# exporter.py
import bpy
import os
import struct
import tempfile
from mathutils import Matrix, Quaternion, Vector

from . import gmd_format as fmt


# ================================================================ Armature
def _find_armature(objects, context):
    """找到一个与 mesh 关联的 armature。"""
    for obj in objects:
        if obj.type != 'MESH':
            continue
        for m in obj.modifiers:
            if m.type == 'ARMATURE' and m.object:
                return m.object
        if obj.parent and obj.parent.type == 'ARMATURE':
            return obj.parent
    for o in context.scene.objects:
        if o.type == 'ARMATURE':
            return o
    return None


def _extract_skeleton(arm_obj):
    """提取骨骼并转换到世界空间坐标（与顶点保持一致）"""
    bones = arm_obj.data.bones
    arm_world = arm_obj.matrix_world
    name_to_idx = {b.name: i for i, b in enumerate(bones)}

    result = []
    ibm = []
    for i, b in enumerate(bones):
        # 世界空间中的绝对 rest 矩阵
        world_mat = arm_world @ b.matrix_local

        # 相对于父骨骼的局部矩阵（仍然用世界矩阵计算，避免空间混淆）
        if b.parent:
            parent_world = arm_world @ b.parent.matrix_local
            local = parent_world.inverted() @ world_mat
        else:
            local = world_mat.copy()

        loc = local.to_translation()
        rot = local.to_quaternion()          # wxyz
        scale = local.to_scale()

        parent_idx = (name_to_idx.get(b.parent.name, fmt.INVALID_INDEX)
                      if b.parent else fmt.INVALID_INDEX)

        result.append({
            'parent_index': parent_idx,
            'name': b.name,
            'translation': (loc.x, loc.y, loc.z),
            'rotation': (rot.x, rot.y, rot.z, rot.w),   # → xyzw
            'scale': (scale.x, scale.y, scale.z),
        })

        # IBM：世界空间 rest 矩阵的逆
        ibm.append(world_mat.inverted())

    return result, ibm


def _build_skel(bones_data, ibm_matrices, add_str):
    if not bones_data:
        return b''

    skel_header_size = 40
    bone_size = 52
    bone_array_off = skel_header_size
    bone_array_bytes = len(bones_data) * bone_size

    ibm_off = fmt.align_up(bone_array_off + bone_array_bytes, 16)
    ibm_bytes = len(ibm_matrices) * 64 if ibm_matrices else 0

    blob = bytearray()
    blob.extend(b'\0' * skel_header_size)

    # 写 Bone 数组
    for b in bones_data:
        name_off = add_str(b['name'])
        blob.extend(struct.pack(
            fmt.BONE_FMT,
            b['parent_index'], name_off,
            b['translation'][0], b['translation'][1], b['translation'][2],
            b['rotation'][0], b['rotation'][1],
            b['rotation'][2], b['rotation'][3],
            b['scale'][0], b['scale'][1], b['scale'][2],
            0))

    # 对齐到 16
    pad = ibm_off - len(blob)
    if pad > 0:
        blob.extend(b'\0' * pad)

    # 写 IBM（列主序：GMD 索引 col*4+row）
    if ibm_matrices:
        for m in ibm_matrices:
            for col in range(4):
                for row in range(4):
                    blob.extend(struct.pack('<f', m[row][col]))

    # 根骨骼索引
    root_bone = 0
    for i, b in enumerate(bones_data):
        if b['parent_index'] == fmt.INVALID_INDEX:
            root_bone = i
            break

    # 回写 Skeleton header
    header = struct.pack(
        fmt.SKELETON_FMT,
        len(bones_data), root_bone,
        bone_array_off, ibm_off, 0,
        0, 0)
    blob[0:skel_header_size] = header

    return bytes(blob)


def _build_anim(armature_obj, add_str):
    """返回 (anim_payload, anim_count)。只导出第一个 active action。"""
    if not armature_obj or not armature_obj.animation_data:
        return b'', 0
    action = armature_obj.animation_data.action
    if not action:
        return b'', 0

    bones = armature_obj.data.bones
    name_to_idx = {b.name: i for i, b in enumerate(bones)}

    # 收集每个骨骼的通道
    ch_map = {}   # (bone_idx, path) -> {array_index: fcurve}
    for fc in action.fcurves:
        dp = fc.data_path
        if not dp.startswith('pose.bones['):
            continue
        try:
            ns = dp.index('"') + 1
            ne = dp.index('"', ns)
        except ValueError:
            continue
        bname = dp[ns:ne]
        if bname not in name_to_idx:
            continue
        bi = name_to_idx[bname]
        prop = dp.rsplit('.', 1)[-1]
        path = {'location': 0, 'rotation_quaternion': 1,
                'scale': 2}.get(prop)
        if path is None:
            continue
        ch_map.setdefault((bi, path), {})[fc.array_index] = fc

    if not ch_map:
        return b'', 0

    # 采样每个通道
    channels = []
    for (bi, path), axes in sorted(ch_map.items()):
        n_comp = 4 if path == 1 else 3
        times_set = set()
        for fc in axes.values():
            for kp in fc.keyframe_points:
                times_set.add(round(kp.co[0], 6))
        times = sorted(times_set)
        if not times:
            continue
        values = []
        for t in times:
            for c in range(n_comp):
                fc = axes.get(c)
                if fc is None:
                    if path == 2:
                        v = 1.0
                    elif path == 1 and c == 3:
                        v = 1.0
                    else:
                        v = 0.0
                else:
                    v = fc.evaluate(t)
                values.append(v)
        channels.append({
            'node_index': bi, 'path': path,
            'interpolation': 0,  # linear
            'times': times, 'values': values,
        })

    if not channels:
        return b'', 0

    # 组装 ANIM 块
    anim_header_size = 32
    ch_array_off = anim_header_size
    ch_array_size = len(channels) * 40

    blob = bytearray()
    blob.extend(b'\0' * anim_header_size)
    ch_placeholder_off = len(blob)
    blob.extend(b'\0' * ch_array_size)

    ch_records = []
    for ch in channels:
        t_off = len(blob)
        for t in ch['times']:
            blob.extend(struct.pack('<f', t))
        v_off = len(blob)
        for v in ch['values']:
            blob.extend(struct.pack('<f', v))
        ch_records.append({
            'node_index': ch['node_index'],
            'path': ch['path'],
            'interpolation': ch['interpolation'],
            'key_count': len(ch['times']),
            'times_offset': t_off,
            'values_offset': v_off,
        })

    for i, rec in enumerate(ch_records):
        off = ch_placeholder_off + i * 40
        blob[off:off + 40] = struct.pack(
            fmt.ANIM_CHANNEL_FMT,
            rec['node_index'], rec['path'], rec['interpolation'],
            rec['key_count'], rec['times_offset'], rec['values_offset'],
            0, 0)

    duration = 0.0
    for ch in channels:
        if ch['times']:
            duration = max(duration, ch['times'][-1])

    anim_name_off = add_str(action.name or 'Action')

    header = struct.pack(
        fmt.ANIMATION_FMT,
        anim_name_off, duration, len(channels), ch_array_off,
        0, 0)
    blob[0:anim_header_size] = header

    return bytes(blob), 1


# ================================================================ 数据收集
def _collect_vertex_skin(mesh, vi, vg_to_bone):
    """返回 (joints[4], weights[4])，已按权重降序取前 4 并归一化。"""
    if not vg_to_bone:
        return (0, 0, 0, 0), (0.0, 0.0, 0.0, 0.0)
    v = mesh.vertices[vi]
    pairs = []
    for g in v.groups:
        bidx = vg_to_bone.get(g.group)
        if bidx is not None and g.weight > 0.0:
            pairs.append((bidx, g.weight))
    if not pairs:
        return (0, 0, 0, 0), (0.0, 0.0, 0.0, 0.0)
    pairs.sort(key=lambda x: -x[1])
    pairs = pairs[:4]
    total = sum(w for _, w in pairs)
    if total <= 0.0:
        return (0, 0, 0, 0), (0.0, 0.0, 0.0, 0.0)
    js = [0, 0, 0, 0]
    ws = [0.0, 0.0, 0.0, 0.0]
    for i, (bidx, w) in enumerate(pairs):
        js[i] = bidx
        ws[i] = w / total
    return tuple(js), tuple(ws)


def _gather(context, objects, apply_modifiers, scale, armature_obj):
    depsgraph = context.evaluated_depsgraph_get()
    verts, norms, uvs, colors, joints, weights = [], [], [], [], [], []
    vmap = {}
    materials, mat_map = [], {}
    tris_by_mat = {}
    used_uv = False
    used_color = False
    used_joints = False

    # 骨骼索引（全局，基于 armature）
    bone_name_to_idx = {}
    if armature_obj:
        for i, b in enumerate(armature_obj.data.bones):
            bone_name_to_idx[b.name] = i

    for obj in objects:
        if obj.type != 'MESH':
            continue

        # 直接使用 obj.data 以避免 armature pose 被烘焙进 rest pose
        mesh = obj.data
        matrix = obj.matrix_world
        try:
            nmat = matrix.to_3x3().inverted().transposed()
        except Exception:
            nmat = matrix.to_3x3()

        uv_layer = mesh.uv_layers.active
        color_layer = None
        try:
            color_layer = mesh.color_attributes.active_color
        except Exception:
            pass

        # 顶点组 -> 骨骼索引 反查
        vg_to_bone = {}
        if armature_obj and len(obj.vertex_groups) > 0:
            vg_name_to_idx = {vg.name: i for i, vg in
                              enumerate(obj.vertex_groups)}
            for bname, bidx in bone_name_to_idx.items():
                if bname in vg_name_to_idx:
                    vg_to_bone[vg_name_to_idx[bname]] = bidx

        for poly in mesh.polygons:
            mat = None
            if poly.material_index < len(mesh.materials):
                mat = mesh.materials[poly.material_index]
            mkey = mat.name if mat else '__default__'
            if mkey not in mat_map:
                mat_map[mkey] = len(materials)
                materials.append(mat)
            mi = mat_map[mkey]

            tri = []
            for li in poly.loop_indices:
                loop = mesh.loops[li]
                vi = loop.vertex_index
                uv = tuple(uv_layer.data[li].uv) if uv_layer else (0.0, 0.0)
                col = (1.0, 1.0, 1.0, 1.0)
                if color_layer is not None:
                    try:
                        c = color_layer.data[li].color
                        col = (c[0], c[1], c[2], c[3])
                    except Exception:
                        pass

                js, ws = _collect_vertex_skin(mesh, vi, vg_to_bone)
                has_skin = any(w > 0.0 for w in ws)

                key = (id(obj), vi,
                       round(uv[0], 6), round(uv[1], 6),
                       round(col[0], 4), round(col[1], 4),
                       round(col[2], 4), round(col[3], 4),
                       js, tuple(round(w, 4) for w in ws))
                if key not in vmap:
                    vmap[key] = len(verts)
                    co = matrix @ mesh.vertices[vi].co
                    verts.append((co.x * scale, co.y * scale, co.z * scale))
                    n = nmat @ mesh.vertices[vi].normal
                    if n.length_squared > 1e-12:
                        n.normalize()
                    norms.append((n.x, n.y, n.z))
                    uvs.append(uv)
                    colors.append(col)
                    joints.append(js)
                    weights.append(ws)
                    if uv != (0.0, 0.0):
                        used_uv = True
                    if col != (1.0, 1.0, 1.0, 1.0):
                        used_color = True
                    if has_skin:
                        used_joints = True
                tri.append(vmap[key])
            for k in range(1, len(tri) - 1):
                tris_by_mat.setdefault(mi, []).append(
                    (tri[0], tri[k], tri[k + 1]))

    return {
        'verts': verts, 'norms': norms, 'uvs': uvs, 'colors': colors,
        'joints': joints, 'weights': weights,
        'materials': materials, 'tris_by_mat': tris_by_mat,
        'used_uv': used_uv, 'used_color': used_color,
        'used_joints': used_joints,
    }


# ================================================================ 主入口
def save(context, filepath, scale=1.0, apply_modifiers=True,
         export_textures=True, embed_textures=True,
         compression='NONE', export_skeleton=True,
         export_animations=True, **kwargs):

    codec = {
        'NONE':    fmt.CompressionCodec.NONE,
        'DEFLATE': fmt.CompressionCodec.DEFLATE,
        'ZSTD':    fmt.CompressionCodec.ZSTD,
        'LZ4':     fmt.CompressionCodec.LZ4,
    }[compression]

    objects = [o for o in context.selected_objects if o.type == 'MESH']
    if not objects:
        objects = [o for o in context.scene.objects if o.type == 'MESH']
    if not objects:
        raise RuntimeError('没有可导出的网格对象')

    armature_obj = _find_armature(objects, context)

    data = _gather(context, objects, apply_modifiers, scale, armature_obj)
    if not data['verts']:
        raise RuntimeError('网格为空')

    # -------- StringTable --------
    string_table = bytearray(b'\0')

    def add_str(s):
        if not s:
            return 0
        key = s.encode('utf-8') + b'\0'
        idx = string_table.find(key, 1)
        if idx >= 0:
            return idx
        off = len(string_table)
        string_table.extend(key)
        return off

    base_name = os.path.splitext(os.path.basename(filepath))[0]
    name_off = add_str(base_name)

    # -------- 骨骼 --------
    bones_data = None
    ibm_matrices = None
    if export_skeleton and armature_obj:
        bones_data, ibm_matrices = _extract_skeleton(armature_obj)

    # -------- 顶点 --------
    vrtx_payload = _build_vrtx(data)

    # -------- 索引 + 子网格 --------
    indx_payload, subm_payload, submesh_count = _build_indx_subm(data)

    # -------- 纹理（先于材质，因为材质需要索引映射）--------
    texi_payload, texd_payload, image_to_idx, tex_count = _build_tex(
        data['materials'], export_textures, add_str)

    # -------- 材质（传入纹理索引映射）--------
    matl_payload = _build_matl(data['materials'], add_str, image_to_idx)

    # -------- 骨骼块 --------
    skel_payload = _build_skel(bones_data, ibm_matrices, add_str) \
        if bones_data else b''

    # -------- 动画块 --------
    anim_payload, anim_count = (b'', 0)
    if export_animations and armature_obj:
        anim_payload, anim_count = _build_anim(armature_obj, add_str)

    # -------- META --------
    skel_count = 1 if (bones_data and skel_payload) else 0
    meta_payload = _build_meta(
        name_off=name_off,
        string_table=bytes(string_table),
        bounds_min=_bounds_min(data['verts']),
        bounds_max=_bounds_max(data['verts']),
        name_length=len(base_name),
        vrtx_count=1,
        indx_count=1,
        submesh_count=submesh_count,
        material_count=len(data['materials']),
        skeleton_count=skel_count,
        animation_count=anim_count,
        texture_count=tex_count,
        sampler_count=0,
    )

    # -------- 块列表 --------
    chunks = [(fmt.CHUNK_META, meta_payload),
              (fmt.CHUNK_VRTX, vrtx_payload),
              (fmt.CHUNK_INDX, indx_payload),
              (fmt.CHUNK_SUBM, subm_payload),
              (fmt.CHUNK_MATL, matl_payload)]
    if skel_payload:
        chunks.append((fmt.CHUNK_SKEL, skel_payload))
    if anim_payload:
        chunks.append((fmt.CHUNK_ANIM, anim_payload))
    if texi_payload:
        chunks.append((fmt.CHUNK_TEXI, texi_payload))
    if texd_payload:
        chunks.append((fmt.CHUNK_TEXD, texd_payload))

    # -------- 写文件 --------
    out = bytearray(fmt.FILE_HEADER_SIZE)
    entries = []

    for fcc, payload in chunks:
        cur = len(out)
        aligned = fmt.align_up(cur, fmt.CHUNK_ALIGNMENT)
        if aligned > cur:
            out.extend(b'\0' * (aligned - cur))
        offset = len(out)

        if codec != fmt.CompressionCodec.NONE:
            comp = fmt.compress(codec, payload)
            if len(comp) < len(payload):
                stored = comp
                flags = int(fmt.ChunkFlags.COMPRESSED)
                stored_codec = int(codec)
            else:
                stored = payload
                flags = 0
                stored_codec = 0
        else:
            stored = payload
            flags = 0
            stored_codec = 0

        out.extend(stored)
        entries.append({
            'type': fcc, 'flags': flags, 'offset': offset,
            'size': len(payload), 'compressed_size': len(stored),
            'alignment': fmt.CHUNK_ALIGNMENT,
            'crc32': fmt.crc32(payload),
            'codec': stored_codec,
        })

    cur = len(out)
    aligned = fmt.align_up(cur, fmt.CHUNK_TABLE_ALIGNMENT)
    if aligned > cur:
        out.extend(b'\0' * (aligned - cur))
    chunk_table_offset = len(out)
    for e in entries:
        out.extend(struct.pack(
            fmt.CHUNK_ENTRY_FMT,
            e['type'], e['flags'], e['offset'], e['size'],
            e['compressed_size'], e['alignment'],
            e['crc32'], e['codec']))

    file_size = len(out)

    file_flags = int(fmt.FileFlags.LITTLE_ENDIAN)
    if texd_payload:
        file_flags |= int(fmt.FileFlags.EMBEDDED_TEX)
    if data['used_joints']:
        pass

    header_pack = struct.pack(
        fmt.FILE_HEADER_FMT,
        fmt.MAGIC, fmt.VERSION_MAJOR, fmt.VERSION_MINOR,
        file_flags, fmt.FILE_HEADER_SIZE, file_size,
        chunk_table_offset, len(entries),
        0, 0,
        b'\0' * 20)
    hcrc = fmt.crc32(header_pack[:36])
    header_pack = (header_pack[:36] +
                   struct.pack('<I', hcrc) +
                   header_pack[40:])
    out[:fmt.FILE_HEADER_SIZE] = header_pack

    with open(filepath, 'wb') as f:
        f.write(out)
    print('[GMD] 已导出: %s (%d bytes, %d chunks, codec=%s, bones=%d, anims=%d, tex=%d)' %
          (filepath, len(out), len(entries), compression,
           len(bones_data) if bones_data else 0, anim_count, tex_count))
    return {'FINISHED'}


# ---------------------------------------------------------------- META
def _build_meta(name_off, string_table, bounds_min, bounds_max,
                name_length, vrtx_count, indx_count, submesh_count,
                material_count, skeleton_count, animation_count,
                texture_count, sampler_count):
    st_offset = 168
    st_size = len(string_table)
    meta = struct.pack(
        fmt.MODEL_META_FMT,
        name_off,
        0, 0, 0, 0, 0, 0, 0,
        st_offset, st_size,
        0, 0,
        bounds_min[0], bounds_min[1], bounds_min[2],
        bounds_max[0], bounds_max[1], bounds_max[2],
        name_length, 0,
        vrtx_count, indx_count, submesh_count, material_count,
        skeleton_count, animation_count, 0,
        texture_count, sampler_count,
        0)
    return meta + string_table


# ---------------------------------------------------------------- VRTX
def _build_vrtx(data):
    A = fmt.VertexAttribute
    V = fmt.VAF

    mask = A.POSITION
    vafs = {0: V.FLOAT32}
    comps = {0: 3}

    if data['norms']:
        mask |= A.NORMAL
        vafs[1] = V.FLOAT32; comps[1] = 3
    if data['used_uv']:
        mask |= A.TEXCOORD0
        vafs[3] = V.FLOAT32; comps[3] = 2
    if data['used_color']:
        mask |= A.COLOR
        vafs[5] = V.UNORM8; comps[5] = 4
    if data['used_joints']:
        mask |= A.JOINTS
        vafs[6] = V.UINT8; comps[6] = 4
        mask |= A.WEIGHTS
        vafs[7] = V.UNORM8; comps[7] = 4

    format_word = fmt.pack_vafs(vafs)

    # 布局
    layout = []
    byte_off = 0
    for s in range(8):
        if not (mask & (1 << s)):
            continue
        vaf = vafs[s]
        n = comps[s]
        info = fmt.vaf_info(vaf)
        ch, sz, norm = info
        layout.append((fmt._SLOT_TO_KEY.get(s, '?'), vaf, n, byte_off,
                       ch, sz, norm))
        byte_off += n * sz

    stride = fmt.align_up(byte_off, 4)

    vb = bytearray()
    for i in range(len(data['verts'])):
        row = bytearray(stride)
        for key, vaf, n, boff, ch, sz, norm in layout:
            if key == 'position':
                vals = data['verts'][i]
            elif key == 'normal':
                vals = data['norms'][i] if i < len(data['norms']) else (0.0, 0.0, 1.0)
            elif key == 'uv0':
                vals = data['uvs'][i] if i < len(data['uvs']) else (0.0, 0.0)
            elif key == 'color':
                vals = data['colors'][i] if i < len(data['colors']) else (1.0, 1.0, 1.0, 1.0)
            elif key == 'joints':
                vals = data['joints'][i] if i < len(data['joints']) else (0, 0, 0, 0)
            elif key == 'weights':
                vals = data['weights'][i] if i < len(data['weights']) else (0.0, 0.0, 0.0, 0.0)
            else:
                vals = (0.0,) * n

            if norm is None:
                struct.pack_into('<' + ch * n, row, boff, *vals)
            else:
                q = []
                for v in vals:
                    iv = int(round(v * norm))
                    if ch == 'b': iv = max(-127, min(127, iv))
                    elif ch == 'B': iv = max(0, min(255, iv))
                    elif ch == 'h': iv = max(-32767, min(32767, iv))
                    elif ch == 'H': iv = max(0, min(65535, iv))
                    q.append(iv)
                struct.pack_into('<' + ch * n, row, boff, *q)
        vb.extend(row)

    bmin = _bounds_min(data['verts'])
    bmax = _bounds_max(data['verts'])

    header = struct.pack(
        fmt.VERTEX_STREAM_FMT,
        0, mask, len(data['verts']), stride,
        72, len(vb),
        format_word, 0,
        bmin[0], bmin[1], bmin[2],
        bmax[0], bmax[1], bmax[2],
        0, 0)
    return header + bytes(vb)


# ---------------------------------------------------------------- INDX + SUBM
def _build_indx_subm(data):
    indices = []
    submeshes = []
    for mi in sorted(data['tris_by_mat'].keys()):
        tris = data['tris_by_mat'][mi]
        offset = len(indices)
        for t in tris:
            indices.extend(t)
        submeshes.append({
            'index_offset': offset, 'index_count': len(tris) * 3,
            'vertex_offset': 0, 'material_index': mi,
            'lod_index': 0, 'flags': 0,
        })

    idx_bytes = struct.pack('<%dI' % len(indices), *indices) if indices else b''
    indx_header = struct.pack(
        fmt.INDEX_BUFFER_FMT,
        len(indices), int(fmt.IndexType.UINT32),
        32, len(idx_bytes), 0, 0)
    indx_payload = indx_header + idx_bytes

    sb = bytearray()
    for sm in submeshes:
        sb.extend(struct.pack(
            fmt.SUBMESH_FMT,
            sm['index_offset'], sm['index_count'], sm['vertex_offset'],
            sm['material_index'], sm['lod_index'], sm['flags'],
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0, 0))
    return indx_payload, bytes(sb), len(submeshes)


# ---------------------------------------------------------------- MATL
def _find_image_for_input(bsdf, input_name):
    """找 BSDF 输入连接的 TexImage 图像。"""
    if input_name not in bsdf.inputs:
        return None
    inp = bsdf.inputs[input_name]
    if not inp.is_linked:
        return None
    node = inp.links[0].from_node
    if node.type == 'TEX_IMAGE' and node.image:
        return node.image
    # 通过 Normal Map 节点
    if node.type == 'NORMAL_MAP':
        c = node.inputs.get('Color')
        if c and c.is_linked:
            inner = c.links[0].from_node
            if inner.type == 'TEX_IMAGE' and inner.image:
                return inner.image
    return None


def _build_matl(blender_mats, add_str, image_to_idx):
    out = bytearray()
    inv = fmt.INVALID_INDEX

    for mat in blender_mats:
        name_off = add_str(mat.name if mat else '')
        bc = (1.0, 1.0, 1.0, 1.0)
        metallic, roughness = 0.0, 0.5
        emissive = (0.0, 0.0, 0.0)
        normal_scale = 1.0

        base_color_ref = (inv, 0, inv, 0)
        mr_ref         = (inv, 0, inv, 0)
        normal_ref     = (inv, 0, inv, 0)
        occ_ref        = (inv, 0, inv, 0)
        emissive_ref   = (inv, 0, inv, 0)

        if mat and mat.use_nodes:
            bsdf = mat.node_tree.nodes.get('Principled BSDF')
            if bsdf:
                try:
                    bc = tuple(bsdf.inputs['Base Color'].default_value)
                except Exception:
                    pass
                try:
                    metallic = bsdf.inputs['Metallic'].default_value
                    roughness = bsdf.inputs['Roughness'].default_value
                except Exception:
                    pass
                if 'Emission' in bsdf.inputs:
                    e = bsdf.inputs['Emission'].default_value
                    emissive = (e[0], e[1], e[2])
                if 'Normal' in bsdf.inputs:
                    ni = bsdf.inputs['Normal']
                    if ni.is_linked and ni.links[0].from_node.type == 'NORMAL_MAP':
                        normal_scale = ni.links[0].from_node.inputs['Strength'].default_value

                img = _find_image_for_input(bsdf, 'Base Color')
                if img:
                    ti = image_to_idx.get(img.name, inv)
                    base_color_ref = (ti, 0, inv, 0)
                img = _find_image_for_input(bsdf, 'Metallic')
                if img is None:
                    img = _find_image_for_input(bsdf, 'Roughness')
                if img:
                    ti = image_to_idx.get(img.name, inv)
                    mr_ref = (ti, 0, inv, 0)
                img = _find_image_for_input(bsdf, 'Normal')
                if img:
                    ti = image_to_idx.get(img.name, inv)
                    normal_ref = (ti, 0, inv, 0)
                img = _find_image_for_input(bsdf, 'Emission')
                if img:
                    ti = image_to_idx.get(img.name, inv)
                    emissive_ref = (ti, 0, inv, 0)

        out.extend(struct.pack(
            fmt.MATERIAL_FMT,
            name_off,
            bc[0], bc[1], bc[2], bc[3],
            metallic, roughness,
            emissive[0], emissive[1], emissive[2],
            normal_scale, 1.0,
            *base_color_ref, *mr_ref, *normal_ref, *occ_ref, *emissive_ref,
            0, 0, 0,
            0, 0))
    return bytes(out)


# ---------------------------------------------------------------- TEXI/TEXD
def _build_tex(blender_mats, export_textures, add_str):
    """返回 (texi_bytes, texd_bytes, image_name_to_idx, tex_count)。"""
    if not export_textures:
        return b'', b'', {}, 0

    images = []
    seen = set()
    for mat in blender_mats:
        if not mat or not mat.use_nodes:
            continue
        for n in mat.node_tree.nodes:
            if n.type == 'TEX_IMAGE' and n.image and n.image.name not in seen:
                seen.add(n.image.name)
                images.append(n.image)

    if not images:
        return b'', b'', {}, 0

    texi = bytearray()
    texd = bytearray()
    image_to_idx = {}

    for idx, img in enumerate(images):
        image_to_idx[img.name] = idx
        name_off = add_str(img.name)
        raw = b''
        try:
            src = bpy.path.abspath(img.filepath_raw) if img.filepath_raw else ''
            if src and os.path.exists(src):
                with open(src, 'rb') as f:
                    raw = f.read()
            else:
                tmp = tempfile.NamedTemporaryFile(suffix='.png', delete=False)
                tmp.close()
                scene = bpy.context.scene
                old = scene.render.image_settings.file_format
                scene.render.image_settings.file_format = 'PNG'
                try:
                    img.save_render(tmp.name)
                finally:
                    scene.render.image_settings.file_format = old
                with open(tmp.name, 'rb') as f:
                    raw = f.read()
                os.unlink(tmp.name)
        except Exception as e:
            print('[GMD] 纹理导出失败 %s: %s' % (img.name, e))

        cur = len(texd)
        aligned = fmt.align_up(cur, fmt.TEXTURE_DATA_ALIGNMENT)
        if aligned > cur:
            texd.extend(b'\0' * (aligned - cur))
        data_off = len(texd)
        texd.extend(raw)

        if raw[:8] == b'\x89PNG\r\n\x1a\n':
            tex_fmt = fmt.TextureFormat.PNG
        elif raw[:2] == b'\xff\xd8':
            tex_fmt = fmt.TextureFormat.JPEG
        elif raw[:4] == b'DDS ':
            tex_fmt = fmt.TextureFormat.DDS
        elif raw[:12] == b'\xabKTX 20\xbb\r\n\x1a\n':
            tex_fmt = fmt.TextureFormat.KTX2
        else:
            tex_fmt = fmt.TextureFormat.UNKNOWN

        w = img.size[0] if img.size[0] > 0 else 1
        h = img.size[1] if img.size[1] > 0 else 1

        texi.extend(struct.pack(
            fmt.TEXTURE_DESC_FMT,
            name_off, 0,
            data_off, len(raw), 0,
            w, h, 1, 1, 1,
            int(tex_fmt),
            fmt.INVALID_INDEX,
            int(fmt.TextureSource.EMBEDDED),
            0,
            fmt.crc32(raw),
            0, 0))

    return bytes(texi), bytes(texd), image_to_idx, len(images)


# ---------------------------------------------------------------- bounds
def _bounds_min(verts):
    if not verts:
        return (0.0, 0.0, 0.0)
    return (min(v[0] for v in verts),
            min(v[1] for v in verts),
            min(v[2] for v in verts))


def _bounds_max(verts):
    if not verts:
        return (0.0, 0.0, 0.0)
    return (max(v[0] for v in verts),
            max(v[1] for v in verts),
            max(v[2] for v in verts))

# importer.py
import bpy
import os
import struct
import tempfile
from mathutils import Vector, Matrix, Quaternion

from . import gmd_format as fmt


# ================================================================ 读取器
class GMDReader:
    def __init__(self, filepath):
        with open(filepath, 'rb') as f:
            self.file_data = f.read()
        self.filepath = filepath
        self.header = {}
        self.chunks = {}
        self.chunk_data = None      # Chunk 数据区（未压缩或解压后）
        self._chunk_cache = {}

        self.meta = None
        self.string_table = b''
        self.string_table_size = 0

        self.vertex_streams = []
        self.vertex_attrs = {}
        self.vertex_count = 0
        self.index_buffers = []      # [{'indices':[...], 'index_type':int}]
        self.submeshes = []
        self.materials = []
        self.textures = []
        self.samplers = []
        self.skeletons = []
        self.animations = []
        self.lods = []

    # -------------------------------------------------------- 入口
    def parse(self):
        self._parse_header()
        self._validate_header_basics()
        self._load_data_area()
        self._parse_chunk_table()
        self._validate_chunks()
        self._parse_meta()
        self._parse_vertex_streams()
        self._parse_vertex_data()
        self._parse_index_buffers()
        self._parse_submeshes()
        self._parse_textures()
        self._parse_samplers()
        self._parse_materials()
        self._parse_skeletons()
        self._parse_animations()
        self._parse_lods()

    # -------------------------------------------------------- Header
    def _parse_header(self):
        if len(self.file_data) < fmt.FILE_HEADER_SIZE:
            raise ValueError('文件小于 FileHeader')
        f = struct.unpack_from(fmt.FILE_HEADER_FMT, self.file_data, 0)
        self.header = {
            'magic': f[0], 'version_major': f[1], 'version_minor': f[2],
            'flags': f[3], 'header_size': f[4], 'file_size': f[5],
            'chunk_table_offset': f[6], 'chunk_count': f[7],
            'header_crc32': f[8], 'codec': f[9], 'reserved': f[10],
        }
        if self.header['magic'] != fmt.MAGIC:
            raise ValueError('魔数错误: 0x%08X' % self.header['magic'])
        if self.header['version_major'] != fmt.VERSION_MAJOR:
            raise ValueError('版本不兼容: %d.%d' %
                             (self.header['version_major'],
                              self.header['version_minor']))
        if self.header['header_size'] != fmt.FILE_HEADER_SIZE:
            raise ValueError('headerSize != 64: %d' % self.header['header_size'])

    def _validate_header_basics(self):
        h = self.header
        if not (h['flags'] & fmt.FileFlags.LITTLE_ENDIAN):
            raise ValueError('未置位 FILE_FLAG_LITTLE_ENDIAN')
        size = len(self.file_data)
        if h['file_size'] != size:
            raise ValueError('fileSize %d != 实际 %d' % (h['file_size'], size))
        if h['chunk_table_offset'] + h['chunk_count'] * fmt.CHUNK_ENTRY_SIZE > size:
            raise ValueError('ChunkTable 越界')
        if h['chunk_table_offset'] % fmt.CHUNK_TABLE_ALIGNMENT != 0:
            raise ValueError('ChunkTable 未按 8B 对齐')
        if h['header_crc32'] != 0:
            if fmt.crc32(self.file_data[:36]) != h['header_crc32']:
                raise ValueError('HeaderCRC32 校验失败')
        # 压缩一致性
        try:
            codec = fmt.CompressionCodec(h['codec'])
        except ValueError:
            raise ValueError('FileHeader.codec 非法: %d' % h['codec'])
        if h['flags'] & fmt.FileFlags.COMPRESSED:
            if codec == fmt.CompressionCodec.NONE:
                raise ValueError('FILE_FLAG_COMPRESSED 置位但 codec == NONE')
            # reserved[8..19] 必须为 0
            if any(self.header['reserved'][8:20]):
                raise ValueError('文件级压缩: reserved[8..19] 必须为 0')
        else:
            if codec != fmt.CompressionCodec.NONE:
                raise ValueError('未设 COMPRESSED 但 codec != NONE')

    def _load_data_area(self):
        h = self.header
        if h['flags'] & fmt.FileFlags.COMPRESSED:
            # reserved[0..7] = compressedDataSize (uint64 LE)
            cds = struct.unpack_from('<Q', h['reserved'], 0)[0]
            if cds == 0:
                raise ValueError('文件级压缩: compressedDataSize == 0')
            if 64 + cds > h['chunk_table_offset']:
                raise ValueError('文件级压缩: 压缩流与 ChunkTable 重叠')
            expected_tbl_off = fmt.align_up(64 + cds, fmt.CHUNK_TABLE_ALIGNMENT)
            if h['chunk_table_offset'] != expected_tbl_off:
                raise ValueError('文件级压缩: ChunkTable 位置不符规范')
            if h['file_size'] != h['chunk_table_offset'] + \
                    h['chunk_count'] * fmt.CHUNK_ENTRY_SIZE:
                raise ValueError('文件级压缩: fileSize 不符')
            raw = self.file_data[64:64 + cds]
            self.chunk_data = fmt.decompress(h['codec'], raw)
            self._file_compressed = True
        else:
            self.chunk_data = self.file_data
            self._file_compressed = False

    # -------------------------------------------------------- ChunkTable
    def _parse_chunk_table(self):
        base = self.header['chunk_table_offset']
        seen_types = set()
        for i in range(self.header['chunk_count']):
            off = base + i * fmt.CHUNK_ENTRY_SIZE
            f = struct.unpack_from(fmt.CHUNK_ENTRY_FMT, self.file_data, off)
            c = {
                'type': f[0], 'flags': f[1], 'offset': f[2], 'size': f[3],
                'compressed_size': f[4], 'alignment': f[5],
                'crc32': f[6], 'codec': f[7],
            }
            if c['type'] in seen_types:
                raise ValueError('同类型块重复: 0x%08X' % c['type'])
            seen_types.add(c['type'])
            self.chunks[c['type']] = c

    def _validate_chunks(self):
        chunk_area_size = len(self.chunk_data)
        table_off = self.header['chunk_table_offset']
        ranges = []
        for typ, c in self.chunks.items():
            align = c['alignment'] or fmt.CHUNK_ALIGNMENT
            if align & (align - 1):
                raise ValueError('块 0x%08X alignment 非 2 的幂: %d' % (typ, align))
            if c['offset'] % align != 0:
                raise ValueError('块 0x%08X 未按 %d 对齐' % (typ, align))
            try:
                codec = fmt.CompressionCodec(c['codec'])
            except ValueError:
                raise ValueError('块 0x%08X codec 非法: %d' % (typ, c['codec']))
            if c['flags'] & fmt.ChunkFlags.COMPRESSED:
                if codec == fmt.CompressionCodec.NONE:
                    raise ValueError('块 0x%08X 置 COMPRESSED 但 codec == NONE' % typ)
                if self._file_compressed:
                    raise ValueError('文件级压缩时块 0x%08X 不允许块级压缩' % typ)
                if c['compressed_size'] == 0:
                    raise ValueError('块 0x%08X compressedSize == 0' % typ)
                csize = c['compressed_size']
            else:
                csize = c['size']
                if c['compressed_size'] != 0 and c['compressed_size'] != c['size']:
                    raise ValueError('块 0x%08X 未压缩但 compressedSize != size' % typ)
            end = c['offset'] + csize
            if end > chunk_area_size:
                raise ValueError('块 0x%08X 数据越界 (end=%d > area=%d)' %
                                 (typ, end, chunk_area_size))
            if not self._file_compressed:
                # 未压缩时，chunk_data 就是整个文件
                if c['offset'] < fmt.FILE_HEADER_SIZE:
                    raise ValueError('块 0x%08X 与 Header 重叠' % typ)
                if end > table_off:
                    raise ValueError('块 0x%08X 与 ChunkTable 重叠' % typ)
            ranges.append((c['offset'], end, typ))
        ranges.sort()
        for i in range(1, len(ranges)):
            if ranges[i][0] < ranges[i - 1][1]:
                raise ValueError('块 0x%08X 与 0x%08X 重叠' %
                                 (ranges[i - 1][2], ranges[i][2]))

    # -------------------------------------------------------- 取块内容
    def _chunk_bytes(self, fcc):
        if fcc in self._chunk_cache:
            return self._chunk_cache[fcc]
        c = self.chunks.get(fcc)
        if not c:
            return None
        off = c['offset']
        if c['flags'] & fmt.ChunkFlags.COMPRESSED:
            raw = self.chunk_data[off:off + c['compressed_size']]
            data = fmt.decompress(c['codec'], raw, c['size'])
        else:
            data = self.chunk_data[off:off + c['size']]
        if c['crc32'] != 0:
            if fmt.crc32(data) != c['crc32']:
                raise ValueError('块 0x%08X CRC 校验失败' % fcc)
        self._chunk_cache[fcc] = data
        return data

    # -------------------------------------------------------- META
    def _parse_meta(self):
        blob = self._chunk_bytes(fmt.CHUNK_META)
        if blob is None:
            raise ValueError('缺少 META 块')
        if len(blob) < 168:
            raise ValueError('META 块小于 ModelMeta')
        f = struct.unpack_from(fmt.MODEL_META_FMT, blob, 0)
        self.meta = {
            'name_offset': f[0],
            'vertex_streams_offset': f[1],
            'index_buffers_offset': f[2],
            'submeshes_offset': f[3],
            'materials_offset': f[4],
            'skeletons_offset': f[5],
            'animations_offset': f[6],
            'lods_offset': f[7],
            'string_table_offset': f[8],
            'string_table_size': f[9],
            'textures_offset': f[10],
            'samplers_offset': f[11],
            'bounds_min': list(f[12:15]),
            'bounds_max': list(f[15:18]),
            'name_length': f[18], 'flags': f[19],
            'vertex_stream_count': f[20],
            'index_buffer_count': f[21],
            'submesh_count': f[22], 'material_count': f[23],
            'skeleton_count': f[24], 'animation_count': f[25],
            'lod_count': f[26], 'texture_count': f[27],
            'sampler_count': f[28],
        }
        st_off = self.meta['string_table_offset']
        st_size = self.meta['string_table_size']
        if st_size < 1:
            raise ValueError('StringTable 至少包含哨兵 \\0')
        if st_off + st_size > len(blob):
            raise ValueError('StringTable 越界')
        self.string_table = blob[st_off:st_off + st_size]
        self.string_table_size = st_size
        if self.string_table[0:1] != b'\0':
            raise ValueError('StringTable[0] 必须为 \\0 哨兵')
        self.meta['name'] = self._str(self.meta['name_offset'])

    def _str(self, offset):
        # 规范 §4(e)：offset == 0 一律表示"无名称"
        if offset == 0:
            return ''
        if offset < 0 or offset >= self.string_table_size:
            return ''
        end = self.string_table.find(b'\0', offset)
        if end < 0:
            return ''
        return self.string_table[offset:end].decode('utf-8', 'replace')

    # -------------------------------------------------------- VRTX
    def _parse_vertex_streams(self):
        blob = self._chunk_bytes(fmt.CHUNK_VRTX)
        if blob is None:
            return
        off0 = self.meta['vertex_streams_offset']
        for i in range(self.meta['vertex_stream_count']):
            off = off0 + i * 72
            if off + 72 > len(blob):
                raise ValueError('VertexStreamDesc 越界')
            f = struct.unpack_from(fmt.VERTEX_STREAM_FMT, blob, off)
            self.vertex_streams.append({
                'stream_index': f[0], 'attribute_mask': f[1],
                'vertex_count': f[2], 'stride': f[3],
                'data_offset': f[4], 'data_size': f[5],
                'format': f[6], 'quantization': f[7],
                'bounds_min': list(f[8:11]), 'bounds_max': list(f[11:14]),
            })
        if self.vertex_streams:
            self.vertex_count = max(s['vertex_count'] for s in self.vertex_streams)

    def _parse_vertex_data(self):
        blob = self._chunk_bytes(fmt.CHUNK_VRTX)
        if blob is None or not self.vertex_streams:
            return
        n = self.vertex_count
        for k in ('position', 'normal', 'tangent',
                  'uv0', 'uv1', 'color', 'joints', 'weights'):
            self.vertex_attrs.setdefault(k, [None] * n)

        for s in self.vertex_streams:
            if s['data_offset'] + s['data_size'] > len(blob):
                raise ValueError('VRTX 顶点数据越界')
            stride = s['stride']
            cnt = s['vertex_count']
            layout = fmt.stream_layout(s['attribute_mask'], s['format'], stride)
            for i in range(cnt):
                voff = s['data_offset'] + i * stride
                for slot, key, boff, vaf, n_comp in layout:
                    info = fmt.vaf_info(vaf)
                    if info is None:
                        continue
                    ch, sz, norm = info
                    if vaf == fmt.VAF.OCT16_2:
                        x_raw, y_raw = struct.unpack_from('<2h', blob, voff + boff)
                        v = fmt.oct16_decode(x_raw, y_raw)
                        if key == 'tangent':
                            self.vertex_attrs[key][i] = (v[0], v[1], v[2], 1.0)
                        else:
                            self.vertex_attrs[key][i] = v
                    else:
                        raw = struct.unpack_from('<' + ch * n_comp, blob, voff + boff)
                        if norm is None:
                            self.vertex_attrs[key][i] = tuple(float(x) for x in raw)
                        else:
                            self.vertex_attrs[key][i] = tuple(x / norm for x in raw)

    # -------------------------------------------------------- INDX
    def _parse_index_buffers(self):
        blob = self._chunk_bytes(fmt.CHUNK_INDX)
        if blob is None:
            return
        off0 = self.meta['index_buffers_offset']
        descs = []
        for i in range(self.meta['index_buffer_count']):
            off = off0 + i * 32
            if off + 32 > len(blob):
                raise ValueError('IndexBufferDesc 越界')
            f = struct.unpack_from(fmt.INDEX_BUFFER_FMT, blob, off)
            descs.append({
                'index_count': f[0], 'index_type': f[1],
                'data_offset': f[2], 'data_size': f[3],
            })
        for d in descs:
            cnt = d['index_count']
            off = d['data_offset']
            if d['index_type'] == fmt.IndexType.UINT16:
                need = cnt * 2
                if off + need > len(blob):
                    raise ValueError('INDX 索引数据越界')
                idx = list(struct.unpack_from('<%dH' % cnt, blob, off)) if cnt else []
            elif d['index_type'] == fmt.IndexType.UINT32:
                need = cnt * 4
                if off + need > len(blob):
                    raise ValueError('INDX 索引数据越界')
                idx = list(struct.unpack_from('<%dI' % cnt, blob, off)) if cnt else []
            else:
                raise ValueError('未知 indexType: %d' % d['index_type'])
            self.index_buffers.append({
                'index_type': d['index_type'],
                'indices': idx,
            })

    # -------------------------------------------------------- SUBM
    def _parse_submeshes(self):
        blob = self._chunk_bytes(fmt.CHUNK_SUBM)
        if blob is None:
            return
        off0 = self.meta['submeshes_offset']
        for i in range(self.meta['submesh_count']):
            off = off0 + i * 56
            if off + 56 > len(blob):
                raise ValueError('Submesh 越界')
            f = struct.unpack_from(fmt.SUBMESH_FMT, blob, off)
            sm = {
                'index_offset': f[0], 'index_count': f[1],
                'vertex_offset': f[2], 'material_index': f[3],
                'lod_index': f[4], 'flags': f[5],
                'bounds_min': list(f[6:9]), 'bounds_max': list(f[9:12]),
                'index_buffer_index': f[12],
            }
            if sm['index_buffer_index'] >= len(self.index_buffers):
                raise ValueError('Submesh.indexBufferIndex 越界')
            if sm['material_index'] >= self.meta['material_count']:
                raise ValueError('Submesh.materialIndex 越界')
            if self.meta['lod_count'] > 0 and sm['lod_index'] >= self.meta['lod_count']:
                raise ValueError('Submesh.lodIndex 越界')
            ib = self.index_buffers[sm['index_buffer_index']]
            if sm['index_offset'] + sm['index_count'] > len(ib['indices']):
                raise ValueError('Submesh 索引范围越界')
            self.submeshes.append(sm)

    # -------------------------------------------------------- TEXI
    def _parse_textures(self):
        blob = self._chunk_bytes(fmt.CHUNK_TEXI)
        if blob is None:
            return
        off0 = self.meta.get('textures_offset', 0)
        for i in range(self.meta['texture_count']):
            off = off0 + i * 88
            if off + 88 > len(blob):
                raise ValueError('TextureDesc 越界')
            f = struct.unpack_from(fmt.TEXTURE_DESC_FMT, blob, off)
            t = {
                'name': self._str(f[0]), 'uri': self._str(f[1]),
                'data_offset': f[2], 'data_size': f[3], 'mips_offset': f[4],
                'width': f[5], 'height': f[6], 'depth': f[7],
                'array_layers': f[8], 'mip_count': f[9],
                'format': f[10], 'sampler_index': f[11],
                'source': f[12], 'flags': f[13], 'crc32': f[14],
                'codec': f[15],
            }
            if t['mip_count'] < 1 or t['array_layers'] < 1 or t['depth'] < 1:
                raise ValueError('TextureDesc 计数非法')
            if t['source'] == fmt.TextureSource.EXTERNAL and t['uri'] == '':
                raise ValueError('外置纹理缺少 uri')
            if t['source'] == fmt.TextureSource.EMBEDDED and t['flags'] & fmt.TextureFlags.COMPRESSED:
                if fmt.CompressionCodec(t['codec']) == fmt.CompressionCodec.NONE:
                    raise ValueError('TEX_FLAG_COMPRESSED 但 codec == NONE')
            if not fmt.is_valid_texture_shape(t['flags'], t['width'], t['height'],
                                              t['depth'], t['array_layers']):
                raise ValueError('TextureDesc 形状非法 (flags=0x%X)' % t['flags'])
            # 解析 mips
            t['mips'] = self._resolve_texture_mips(blob, t, off0)
            self.textures.append(t)

    def _resolve_texture_mips(self, texi_blob, t, texi_off0):
        """返回 [(offset, size, width, height)]，offset 相对 TEXD 起始。"""
        mips = []
        if t['mips_offset'] != 0:
            base = t['mips_offset']
            for i in range(t['mip_count']):
                off = base + i * 24
                if off + 24 > len(texi_blob):
                    raise ValueError('TextureMipInfo 越界')
                mf = struct.unpack_from(fmt.TEXTURE_MIP_FMT, texi_blob, off)
                if mf[0] % fmt.TEXTURE_DATA_ALIGNMENT != 0:
                    raise ValueError('TextureMipInfo.offset 未按 256 对齐')
                mips.append((mf[0], mf[1], mf[2], mf[3]))
        else:
            if fmt.is_container_format(t['format']):
                # 容器格式：单个数据块
                mips.append((t['data_offset'], t['data_size'],
                             t['width'], t['height']))
            elif fmt.is_pixel_format(t['format']) or fmt.is_gpu_compressed_format(t['format']):
                offsets = fmt.compute_mip_offsets(
                    t['format'], t['width'], t['height'],
                    t['depth'], t['array_layers'], t['mip_count'])
                for (rel_off, sz, w, h) in offsets:
                    mips.append((t['data_offset'] + rel_off, sz, w, h))
            else:
                # 未知格式，按单块处理
                mips.append((t['data_offset'], t['data_size'],
                             t['width'], t['height']))
        return mips

    def _parse_samplers(self):
        blob = self._chunk_bytes(fmt.CHUNK_SMPL)
        if blob is None:
            return
        off0 = self.meta.get('samplers_offset', 0)
        for i in range(self.meta['sampler_count']):
            off = off0 + i * 56
            if off + 56 > len(blob):
                raise ValueError('SamplerDesc 越界')
            f = struct.unpack_from(fmt.SAMPLER_DESC_FMT, blob, off)
            self.samplers.append({
                'name': self._str(f[0]),
                'min_filter': f[1], 'mag_filter': f[2], 'mip_filter': f[3],
                'wrap_s': f[4], 'wrap_t': f[5], 'wrap_r': f[6],
                'lod_bias': f[7], 'max_anisotropy': f[8],
                'compare_func': f[9], 'flags': f[10],
            })

    # -------------------------------------------------------- MATL
    def _parse_materials(self):
        blob = self._chunk_bytes(fmt.CHUNK_MATL)
        if blob is None:
            return
        off0 = self.meta['materials_offset']
        for i in range(self.meta['material_count']):
            off = off0 + i * 152
            if off + 152 > len(blob):
                raise ValueError('Material 越界')
            f = struct.unpack_from(fmt.MATERIAL_FMT, blob, off)
            idx = 1
            bc = list(f[idx:idx + 4]); idx += 4
            metallic = f[idx]; idx += 1
            roughness = f[idx]; idx += 1
            emissive = list(f[idx:idx + 3]); idx += 3
            normal_scale = f[idx]; idx += 1
            occ = f[idx]; idx += 1
            refs = []
            for _ in range(5):
                refs.append({
                    'texture_index': f[idx], 'uv_set': f[idx + 1],
                    'sampler_index': f[idx + 2], 'flags': f[idx + 3],
                })
                idx += 4
            # 校验引用
            for ref in refs:
                ti = ref['texture_index']
                if ti != fmt.INVALID_INDEX and ti >= self.meta['texture_count']:
                    raise ValueError('Material TextureRef.textureIndex 越界')
                si = ref['sampler_index']
                if si != fmt.INVALID_INDEX and si >= self.meta['sampler_count']:
                    raise ValueError('Material TextureRef.samplerIndex 越界')
            self.materials.append({
                'name': self._str(f[0]),
                'base_color_factor': bc,
                'metallic_factor': metallic, 'roughness_factor': roughness,
                'emissive_factor': emissive,
                'normal_scale': normal_scale, 'occlusion_strength': occ,
                'base_color_texture': refs[0],
                'metallic_roughness_texture': refs[1],
                'normal_texture': refs[2],
                'occlusion_texture': refs[3],
                'emissive_texture': refs[4],
                'flags': f[idx],
                'custom_data_offset': f[idx + 1],
                'custom_data_size': f[idx + 2],
            })

    # -------------------------------------------------------- SKEL
    def _parse_skeletons(self):
        blob = self._chunk_bytes(fmt.CHUNK_SKEL)
        if blob is None:
            return
        off0 = self.meta['skeletons_offset']
        for i in range(self.meta['skeleton_count']):
            off = off0 + i * 40
            if off + 40 > len(blob):
                raise ValueError('Skeleton 越界')
            f = struct.unpack_from(fmt.SKELETON_FMT, blob, off)
            bones = []
            bone_off = f[2]
            for j in range(f[0]):
                bo = bone_off + j * 52
                if bo + 52 > len(blob):
                    raise ValueError('Bone 越界')
                bf = struct.unpack_from(fmt.BONE_FMT, blob, bo)
                bones.append({
                    'parent_index': bf[0], 'name': self._str(bf[1]),
                    'translation': list(bf[2:5]),
                    'rotation': list(bf[5:9]),   # xyzw
                    'scale': list(bf[9:12]),
                })
            # 逆绑定矩阵
            ibm = None
            ibm_off = f[3]
            if ibm_off != 0:
                if ibm_off % 16 != 0:
                    raise ValueError('inverseBindMatrices 未 16 字节对齐')
                need = f[0] * 16 * 4
                if ibm_off + need > len(blob):
                    raise ValueError('inverseBindMatrices 越界')
                floats = struct.unpack_from('<%df' % (f[0] * 16), blob, ibm_off)
                ibm = []
                for k in range(f[0]):
                    m = floats[k * 16:(k + 1) * 16]
                    # 列主序 → mathutils.Matrix（列主序构造）
                    mat = Matrix((
                        (m[0], m[4], m[8],  m[12]),
                        (m[1], m[5], m[9],  m[13]),
                        (m[2], m[6], m[10], m[14]),
                        (m[3], m[7], m[11], m[15]),
                    ))
                    ibm.append(mat)
            # 层级顺序
            hierarchy = None
            nh_off = f[4]
            if nh_off != 0:
                need = f[0] * 4
                if nh_off + need > len(blob):
                    raise ValueError('nodeHierarchy 越界')
                hierarchy = list(struct.unpack_from('<%dI' % f[0], blob, nh_off))
            self.skeletons.append({
                'bone_count': f[0], 'root_bone': f[1], 'bones': bones,
                'inverse_bind_matrices': ibm,
                'node_hierarchy': hierarchy,
            })

    # -------------------------------------------------------- ANIM
    def _parse_animations(self):
        blob = self._chunk_bytes(fmt.CHUNK_ANIM)
        if blob is None:
            return
        off0 = self.meta['animations_offset']
        bone_count = (self.skeletons[0]['bone_count']
                      if self.skeletons else 0)
        for i in range(self.meta['animation_count']):
            off = off0 + i * 32
            if off + 32 > len(blob):
                raise ValueError('Animation 越界')
            f = struct.unpack_from(fmt.ANIMATION_FMT, blob, off)
            channels = []
            ch_off = f[3]
            for j in range(f[2]):
                co = ch_off + j * 40
                if co + 40 > len(blob):
                    raise ValueError('AnimationChannel 越界')
                cf = struct.unpack_from(fmt.ANIM_CHANNEL_FMT, blob, co)
                if bone_count and cf[0] >= bone_count:
                    raise ValueError('AnimationChannel.nodeIndex 越界')
                n_comp = {0: 3, 1: 4, 2: 3}.get(cf[1], 3)
                times, values = [], []
                in_tangents, out_tangents = [], []
                if cf[3] > 0:
                    t_off = cf[4]
                    if t_off + cf[3] * 4 > len(blob):
                        raise ValueError('Anim times 越界')
                    times = list(struct.unpack_from('<%df' % cf[3], blob, t_off))
                    # 校验严格递增
                    for k in range(1, len(times)):
                        if times[k] <= times[k - 1]:
                            raise ValueError('Animation times 未严格递增')
                    v_off = cf[5]
                    v_size = cf[3] * n_comp * 4
                    if v_off + v_size > len(blob):
                        raise ValueError('Anim values 越界')
                    values = list(struct.unpack_from(
                        '<%df' % (cf[3] * n_comp), blob, v_off))
                    if cf[2] == int(fmt.AnimationInterpolation.CUBIC):
                        in_off = v_off + v_size
                        out_off = in_off + v_size
                        if out_off + v_size > len(blob):
                            raise ValueError('Anim cubic tangents 越界')
                        in_tangents = list(struct.unpack_from(
                            '<%df' % (cf[3] * n_comp), blob, in_off))
                        out_tangents = list(struct.unpack_from(
                            '<%df' % (cf[3] * n_comp), blob, out_off))
                channels.append({
                    'node_index': cf[0], 'path': cf[1],
                    'interpolation': cf[2], 'key_count': cf[3],
                    'times': times, 'values': values,
                    'in_tangents': in_tangents, 'out_tangents': out_tangents,
                })
            self.animations.append({
                'name': self._str(f[0]), 'duration': f[1], 'channels': channels,
            })

    # -------------------------------------------------------- LODS
    def _parse_lods(self):
        blob = self._chunk_bytes(fmt.CHUNK_LODS)
        if blob is None:
            return
        off0 = self.meta['lods_offset']
        for i in range(self.meta['lod_count']):
            off = off0 + i * 16
            if off + 16 > len(blob):
                raise ValueError('LodDesc 越界')
            f = struct.unpack_from(fmt.LOD_DESC_FMT, blob, off)
            if f[1] + f[2] > self.meta['submesh_count']:
                raise ValueError('LodDesc 子网格范围越界')
            self.lods.append({
                'distance': f[0], 'submesh_start': f[1], 'submesh_count': f[2],
            })


# ================================================================ 图像加载
def _image_from_bytes(name, raw):
    if raw[:8] == b'\x89PNG\r\n\x1a\n':   ext = '.png'
    elif raw[:2] == b'\xff\xd8':           ext = '.jpg'
    elif raw[:4] == b'DDS ':               ext = '.dds'
    elif raw[:12] == b'\xabKTX 20\xbb\r\n\x1a\n': ext = '.ktx2'
    else:                                  ext = '.bin'
    tmp = tempfile.NamedTemporaryFile(suffix=ext, delete=False)
    try:
        tmp.write(raw); tmp.close()
        img = bpy.data.images.load(tmp.name, check_existing=False)
        img.name = name
        try: img.pack()
        except Exception: pass
        return img
    except Exception as e:
        print('[GMD] 内嵌纹理 %s 加载失败: %s' % (name, e))
        return None
    finally:
        try: os.unlink(tmp.name)
        except OSError: pass


def _image_from_pixels(name, width, height, pix_bytes, fmt_code):
    """从原始像素数据构建 Blender 图像（仅支持简单 8 位格式）。"""
    try:
        tfmt = fmt.TextureFormat(fmt_code)
    except ValueError:
        return None
    # 转 RGBA8
    if tfmt == fmt.TextureFormat.RGBA8_UNORM or tfmt == fmt.TextureFormat.RGBA8_SRGB:
        rgba = bytearray(pix_bytes)
    elif tfmt == fmt.TextureFormat.BGRA8_UNORM or tfmt == fmt.TextureFormat.BGRA8_SRGB:
        rgba = bytearray(len(pix_bytes))
        for i in range(0, len(pix_bytes), 4):
            rgba[i + 0] = pix_bytes[i + 2]
            rgba[i + 1] = pix_bytes[i + 1]
            rgba[i + 2] = pix_bytes[i + 0]
            rgba[i + 3] = pix_bytes[i + 3]
    elif tfmt == fmt.TextureFormat.R8_UNORM:
        rgba = bytearray(width * height * 4)
        for i in range(width * height):
            v = pix_bytes[i]
            rgba[i*4:i*4+4] = bytes((v, v, v, 255))
    elif tfmt == fmt.TextureFormat.RG8_UNORM:
        rgba = bytearray(width * height * 4)
        for i in range(width * height):
            rgba[i*4+0] = pix_bytes[i*2+0]
            rgba[i*4+1] = pix_bytes[i*2+1]
            rgba[i*4+2] = 0
            rgba[i*4+3] = 255
    else:
        return None

    # Blender 图像坐标原点在左下；GMD 使用左上（图像坐标），需翻转
    need_flip = True
    if need_flip:
        row = width * 4
        flipped = bytearray(len(rgba))
        for y in range(height):
            src = y * row
            dst = (height - 1 - y) * row
            flipped[dst:dst + row] = rgba[src:src + row]
        rgba = flipped

    img = bpy.data.images.new(name, width, height, alpha=True)
    # 转为 float 数组
    pixels = [0.0] * (width * height * 4)
    for i in range(0, len(rgba), 4):
        pixels[i + 0] = rgba[i + 0] / 255.0
        pixels[i + 1] = rgba[i + 1] / 255.0
        pixels[i + 2] = rgba[i + 2] / 255.0
        pixels[i + 3] = rgba[i + 3] / 255.0
    img.pixels = pixels
    if tfmt in (fmt.TextureFormat.RGBA8_SRGB, fmt.TextureFormat.BGRA8_SRGB):
        try:
            img.colorspace_settings.name = 'sRGB'
        except Exception:
            pass
    else:
        try:
            img.colorspace_settings.name = 'Non-Color'
        except Exception:
            pass
    return img


# ================================================================ 主入口
def load(context, filepath, scale=1.0, uv_flip_mode='AUTO',
         import_textures=True, import_skeleton=True,
         import_animations=True, import_lods=False, **kwargs):

    reader = GMDReader(filepath)
    reader.parse()

    file_flip = bool(reader.header['flags'] & fmt.FileFlags.UV_TOP_LEFT)
    flip_v = {'AUTO': file_flip, 'FLIP': True, 'NO_FLIP': False}[uv_flip_mode]

    coll = context.collection or context.scene.collection
    base_name = reader.meta.get('name') or os.path.splitext(
        os.path.basename(filepath))[0]

    created = []
    blender_mats = _build_materials(reader, import_textures)
    mesh_obj = _build_mesh(reader, base_name, blender_mats, scale, flip_v,
                           import_lods)
    if mesh_obj:
        coll.objects.link(mesh_obj)
        created.append(mesh_obj)

    arm_obj = None
    if import_skeleton and reader.skeletons:
        try:
            arm_obj = _build_armature(reader, base_name, scale, coll)
        except Exception as e:
            print('[GMD] 构建骨架失败:', e)
            import traceback
            traceback.print_exc()
            arm_obj = None

        if arm_obj:
            # 注意：_build_armature 里已 link，此处不再 link
            created.append(arm_obj)
            if mesh_obj:
                try:
                    _bind_skin(mesh_obj, arm_obj, reader)
                except Exception as e:
                    print('[GMD] 蒙皮绑定失败:', e)
                    import traceback
                    traceback.print_exc()

    if import_animations and reader.animations and arm_obj:
        try:
            _build_animations(reader, arm_obj)
        except Exception as e:
            print('[GMD] 构建动画失败:', e)
            import traceback
            traceback.print_exc()

    bpy.ops.object.select_all(action='DESELECT')
    for o in created:
        o.select_set(True)
    if created:
        context.view_layer.objects.active = created[0]
    return {'FINISHED'}

# ---------------------------------------------------------------- 材质
def _build_materials(reader, import_textures):
    images = {}
    if import_textures:
        texd = reader._chunk_bytes(fmt.CHUNK_TEXD)
        for ti, tex in enumerate(reader.textures):
            img = None
            if tex['source'] == fmt.TextureSource.EXTERNAL and tex['uri']:
                p = tex['uri']
                if not os.path.isabs(p):
                    p = os.path.join(os.path.dirname(reader.filepath), p)
                if os.path.exists(p):
                    try:
                        img = bpy.data.images.load(p, check_existing=True)
                    except Exception as e:
                        print('[GMD] 外部纹理失败:', e)
            elif tex['source'] == fmt.TextureSource.EMBEDDED and texd:
                img = _load_embedded_texture(reader, texd, tex, ti)
            if img:
                images[ti] = img

    out = []
    for mi, mat in enumerate(reader.materials):
        bmat = bpy.data.materials.new(mat['name'] or ('Material_%d' % mi))
        bmat.use_nodes = True
        nt = bmat.node_tree
        bsdf = nt.nodes.get('Principled BSDF')
        if bsdf is None:
            out.append(bmat); continue

        bc = mat['base_color_factor']
        bsdf.inputs['Base Color'].default_value = bc
        bsdf.inputs['Metallic'].default_value = mat['metallic_factor']
        bsdf.inputs['Roughness'].default_value = mat['roughness_factor']
        em = mat['emissive_factor']
        if 'Emission' in bsdf.inputs:
            bsdf.inputs['Emission'].default_value = (em[0], em[1], em[2], 1.0)

        def hook(ref, input_name, colorspace):
            idx = ref['texture_index']
            if idx == fmt.INVALID_INDEX or idx not in images:
                return
            img = images[idx]
            try:
                img.colorspace_settings.name = colorspace
            except Exception:
                pass
            node = nt.nodes.new('ShaderNodeTexImage')
            node.image = img
            node.location = (-400, -250 * len(nt.nodes))
            nt.links.new(node.outputs['Color'], bsdf.inputs[input_name])

        hook(mat['base_color_texture'], 'Base Color', 'sRGB')
        hook(mat['metallic_roughness_texture'], 'Metallic', 'Non-Color')
        hook(mat['normal_texture'], 'Normal', 'Non-Color')
        hook(mat['emissive_texture'], 'Emission', 'sRGB')
        out.append(bmat)
    return out


def _load_embedded_texture(reader, texd, tex, ti):
    off = tex['data_offset']
    size = tex['data_size']
    if size == 0 or off + size > len(texd):
        return None
    raw = texd[off:off + size]
    # 纹理级压缩
    if tex['flags'] & fmt.TextureFlags.COMPRESSED:
        try:
            # 解压后大小由格式和 shape 推导
            if fmt.is_pixel_format(tex['format']) or fmt.is_gpu_compressed_format(tex['format']):
                expected = fmt.compute_pixel_data_size(
                    tex['format'], tex['width'], tex['height'],
                    tex['depth'], tex['array_layers'], tex['mip_count'])
            else:
                expected = None
            raw = fmt.decompress(tex['codec'], raw, expected)
        except Exception as e:
            print('[GMD] 纹理 %d 解压失败: %s' % (ti, e))
            return None
    if tex['crc32'] and raw:
        if fmt.crc32(raw) != tex['crc32']:
            print('[GMD] 纹理 %d CRC 不匹配' % ti)

    if fmt.is_container_format(tex['format']):
        return _image_from_bytes(tex['name'] or ('gmd_tex_%d' % ti), raw)
    elif fmt.is_pixel_format(tex['format']):
        # 提取 mip0
        mips = tex.get('mips', [])
        if not mips:
            return None
        # mip0 的 offset 相对 TEXD 起始，需减去 data_offset
        m0_off = mips[0][0] - tex['data_offset']
        m0_size = mips[0][1]
        if m0_off < 0 or m0_off + m0_size > len(raw):
            return None
        pix = raw[m0_off:m0_off + m0_size]
        return _image_from_pixels(
            tex['name'] or ('gmd_tex_%d' % ti),
            tex['width'], tex['height'], pix, tex['format'])
    return None


# ---------------------------------------------------------------- 网格
def _build_mesh(reader, base_name, blender_mats, scale, flip_v, import_lods):
    n = reader.vertex_count
    if n == 0 or not reader.submeshes:
        return None

    raw_pos = reader.vertex_attrs.get('position') or [(0.0, 0.0, 0.0)] * n
    raw_nrm = reader.vertex_attrs.get('normal')
    raw_uv0 = reader.vertex_attrs.get('uv0')
    raw_col = reader.vertex_attrs.get('color')

    verts = []
    for p in raw_pos:
        p = p or (0.0, 0.0, 0.0)
        verts.append((p[0] * scale, p[1] * scale, p[2] * scale))

    faces, face_mats = [], []
    for sm in reader.submeshes:
        if not import_lods and sm['lod_index'] != 0:
            continue
        ib = reader.index_buffers[sm['index_buffer_index']]
        start = sm['index_offset']
        cnt = sm['index_count']
        idx = ib['indices'][start:start + cnt]
        mat_idx = sm['material_index']
        for k in range(0, cnt - 2, 3):
            a, b, c = idx[k], idx[k + 1], idx[k + 2]
            if a == b or b == c or a == c:
                continue
            if a >= n or b >= n or c >= n:
                continue
            faces.append((a, b, c))
            face_mats.append(mat_idx)

    mesh = bpy.data.meshes.new(base_name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()

    if raw_uv0 and any(u is not None for u in raw_uv0):
        uvl = mesh.uv_layers.new(name='UVMap')
        for loop in mesh.loops:
            u = raw_uv0[loop.vertex_index] or (0.0, 0.0)
            v = (1.0 - u[1]) if flip_v else u[1]
            uvl.data[loop.index].uv = (u[0], v)

    if raw_col and any(c is not None for c in raw_col):
        try:
            vc = mesh.color_attributes.new(name='Col', type='BYTE_COLOR',
                                           domain='CORNER')
            for loop in mesh.loops:
                c = raw_col[loop.vertex_index] or (1.0, 1.0, 1.0, 1.0)
                rgba = (c[0], c[1], c[2], c[3] if len(c) > 3 else 1.0)
                vc.data[loop.index].color = rgba
        except Exception as e:
            print('[GMD] 顶点色失败:', e)

    if raw_nrm and any(x is not None for x in raw_nrm):
        try:
            loops = []
            for loop in mesh.loops:
                nn = raw_nrm[loop.vertex_index]
                if nn is None:
                    nn = (0.0, 0.0, 1.0)
                loops.append((nn[0], nn[1], nn[2]))
            mesh.normals_split_custom_set(loops)
        except Exception as e:
            print('[GMD] 自定义法线失败:', e)

    for bm in blender_mats:
        mesh.materials.append(bm)
    for i, poly in enumerate(mesh.polygons):
        if i < len(face_mats):
            mi = face_mats[i]
            if 0 <= mi < len(blender_mats):
                poly.material_index = mi
        poly.use_smooth = True

    return bpy.data.objects.new(base_name, mesh)


# ---------------------------------------------------------------- 骨架
def _build_armature(reader, base_name, scale, coll):
    """构建骨架并 link 到集合。骨骼位置按导入 scale 缩放。"""
    sk = reader.skeletons[0]
    bones_data = sk['bones']
    if not bones_data:
        return None

    arm_data = bpy.data.armatures.new(base_name + '_Armature')
    arm_obj = bpy.data.objects.new(base_name + '_Armature', arm_data)
    coll.objects.link(arm_obj)

    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = arm_obj
    arm_obj.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')

    # 计算每个骨骼的绝对世界矩阵（local T/R/S 逐级累乘）
    world = [None] * len(bones_data)

    def compute_world(idx):
        if world[idx] is not None:
            return world[idx]
        b = bones_data[idx]
        t = Vector(b['translation'])
        r = b['rotation']              # xyzw
        q = Quaternion((r[3], r[0], r[1], r[2]))  # Blender wxyz
        s = Vector(b['scale'])
        local = (Matrix.Translation(t) @
                 q.to_matrix().to_4x4() @
                 Matrix.Diagonal(s.to_4d()))
        p = b['parent_index']
        if p == fmt.INVALID_INDEX or p >= len(bones_data):
            w = local
        else:
            w = compute_world(p) @ local
        world[idx] = w
        return w

    for i in range(len(bones_data)):
        compute_world(i)

    ebs = []
    used_names = set()
    for i, bone in enumerate(bones_data):
        name = bone['name'] or ('Bone_%d' % i)
        base = name
        k = 0
        while name in used_names:
            k += 1
            name = '%s_%d' % (base, k)
        used_names.add(name)

        eb = arm_data.edit_bones.new(name)
        wm = world[i]
        head = wm.to_translation()
        y_axis = wm.to_3x3().col[1]
        if y_axis.length < 1e-6:
            y_axis = Vector((0.0, 1.0, 0.0))
        else:
            y_axis.normalize()

        # scale 参数缩放 head/tail（bone length 也随之按 scale 缩放）
        eb.head = head * scale
        eb.tail = (head + y_axis * 0.05) * scale
        ebs.append(eb)

    # 设置父子关系
    for i, bone in enumerate(bones_data):
        p = bone['parent_index']
        if p != fmt.INVALID_INDEX and p < len(ebs):
            ebs[i].parent = ebs[p]
            ebs[i].use_connect = False

    bpy.ops.object.mode_set(mode='OBJECT')

    return arm_obj

def _bind_skin(mesh_obj, arm_obj, reader):
    joints = reader.vertex_attrs.get('joints')
    weights = reader.vertex_attrs.get('weights')
    if not joints or not weights:
        print('[GMD] 无 joints/weights 顶点属性，跳过蒙皮')
        return

    has_data = any(js and ws and any(w > 0.0 for w in ws)
                   for js, ws in zip(joints, weights))
    if not has_data:
        print('[GMD] joints/weights 全为 0，跳过蒙皮')
        return

    # Armature 修改器 + 父级
    mod = mesh_obj.modifiers.new(name='Armature', type='ARMATURE')
    mod.object = arm_obj

    # 记录原始世界矩阵
    mw = mesh_obj.matrix_world.copy()
    mesh_obj.parent = arm_obj
    # 保持视觉位置不变
    mesh_obj.matrix_parent_inverse = arm_obj.matrix_world.inverted()
    mesh_obj.matrix_world = mw

    bones = arm_obj.data.bones
    if not bones:
        return

    # 清空旧顶点组
    while mesh_obj.vertex_groups:
        mesh_obj.vertex_groups.remove(mesh_obj.vertex_groups[0])

    # 按骨骼索引创建顶点组（顺序严格按 GMD Bone[] 数组）
    groups_by_idx = {}
    for i, b in enumerate(bones):
        groups_by_idx[i] = mesh_obj.vertex_groups.new(name=b.name)

    bound = 0
    for vi, (js, ws) in enumerate(zip(joints, weights)):
        if js is None or ws is None:
            continue
        for k in range(min(4, len(js), len(ws))):
            bi = int(js[k])
            w = float(ws[k])
            if w <= 0.0 or bi not in groups_by_idx:
                continue
            try:
                groups_by_idx[bi].add([vi], w, 'REPLACE')
                bound += 1
            except Exception:
                pass
    print('[GMD] 蒙皮: %d 顶点组, %d 权重赋值' %
          (len(groups_by_idx), bound))

# ---------------------------------------------------------------- 动画
def _build_animations(reader, arm_obj):
    if arm_obj.animation_data is None:
        arm_obj.animation_data_create()
    bones = arm_obj.data.bones
    for anim in reader.animations:
        action = bpy.data.actions.new(anim['name'] or 'Action')
        arm_obj.animation_data.action = action
        for ch in anim['channels']:
            ni = ch['node_index']
            if ni >= len(bones):
                continue
            path_map = {0: 'location', 1: 'rotation_quaternion', 2: 'scale'}
            dp = 'pose.bones["%s"].%s' % (bones[ni].name, path_map[ch['path']])
            n_comp = {0: 3, 1: 4, 2: 3}[ch['path']]
            interp = 'LINEAR'
            if ch['interpolation'] == int(fmt.AnimationInterpolation.STEP):
                interp = 'CONSTANT'
            elif ch['interpolation'] == int(fmt.AnimationInterpolation.CUBIC):
                interp = 'BEZIER'
            for k in range(ch['key_count']):
                frame = int(round(ch['times'][k] * 24.0)) + 1
                vals = ch['values'][k * n_comp:(k + 1) * n_comp]
                for c in range(n_comp):
                    fc = action.fcurves.find(dp, index=c)
                    if fc is None:
                        fc = action.fcurves.new(dp, index=c,
                                                action_group=bones[ni].name)
                    kp = fc.keyframe_points.insert(frame, vals[c])
                    kp.interpolation = interp
        for fc in action.fcurves:
            fc.update()

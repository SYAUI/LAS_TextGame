# gmd_format.py
"""GMD 二进制格式定义 — 严格对齐 gmd.h v1.0"""
import struct
import zlib
from enum import IntEnum

MAGIC = 0x42444D47
VERSION_MAJOR = 1
VERSION_MINOR = 0
FILE_HEADER_SIZE = 64
CHUNK_ENTRY_SIZE = 48
CHUNK_ALIGNMENT = 64
CHUNK_TABLE_ALIGNMENT = 8
TEXTURE_DATA_ALIGNMENT = 256
INVALID_INDEX = 0xFFFFFFFF


def fourcc(s):
    return ord(s[0]) | (ord(s[1]) << 8) | (ord(s[2]) << 16) | (ord(s[3]) << 24)


CHUNK_META = fourcc('META'); CHUNK_VRTX = fourcc('VRTX')
CHUNK_INDX = fourcc('INDX'); CHUNK_SUBM = fourcc('SUBM')
CHUNK_MATL = fourcc('MATL'); CHUNK_SKEL = fourcc('SKEL')
CHUNK_ANIM = fourcc('ANIM'); CHUNK_LODS = fourcc('LODS')
CHUNK_TEXI = fourcc('TEXI'); CHUNK_TEXD = fourcc('TEXD')
CHUNK_SMPL = fourcc('SMPL')

_CHUNK_NAMES = {
    CHUNK_META: 'META', CHUNK_VRTX: 'VRTX', CHUNK_INDX: 'INDX',
    CHUNK_SUBM: 'SUBM', CHUNK_MATL: 'MATL', CHUNK_SKEL: 'SKEL',
    CHUNK_ANIM: 'ANIM', CHUNK_LODS: 'LODS', CHUNK_TEXI: 'TEXI',
    CHUNK_TEXD: 'TEXD', CHUNK_SMPL: 'SMPL',
}


def chunk_name(t):
    return _CHUNK_NAMES.get(t, '0x%08X' % t)


class FileFlags(IntEnum):
    LITTLE_ENDIAN = 1 << 0
    COMPRESSED    = 1 << 1
    EMBEDDED_TEX  = 1 << 2
    QUANTIZED     = 1 << 3
    UV_TOP_LEFT   = 1 << 4


class ChunkFlags(IntEnum):
    COMPRESSED = 1 << 0
    OPTIONAL   = 1 << 1


class VertexAttribute(IntEnum):
    POSITION  = 1 << 0
    NORMAL    = 1 << 1
    TANGENT   = 1 << 2
    TEXCOORD0 = 1 << 3
    TEXCOORD1 = 1 << 4
    COLOR     = 1 << 5
    JOINTS    = 1 << 6
    WEIGHTS   = 1 << 7


class VAF(IntEnum):
    UNUSED   = 0
    FLOAT32  = 1
    FLOAT16  = 2
    SINT8    = 3
    SNORM8   = 4
    UINT8    = 5
    UNORM8   = 6
    SINT16   = 7
    SNORM16  = 8
    UINT16   = 9
    UNORM16  = 10
    OCT16_2  = 11


class IndexType(IntEnum):
    UINT16 = 0
    UINT32 = 1


class CompressionCodec(IntEnum):
    NONE = 0
    ZSTD = 1
    LZ4 = 2
    DEFLATE = 3


class TextureSource(IntEnum):
    EMBEDDED = 0
    EXTERNAL = 1


class TextureFormat(IntEnum):
    UNKNOWN = 0
    R8_UNORM = 1
    RG8_UNORM = 2
    RGBA8_UNORM = 3
    RGBA8_SRGB = 4
    BGRA8_UNORM = 5
    BGRA8_SRGB = 6
    RGBA16_FLOAT = 7
    RGBA32_FLOAT = 8
    BC1_UNORM = 16; BC1_SRGB = 17
    BC3_UNORM = 18; BC3_SRGB = 19
    BC4_UNORM = 20; BC5_UNORM = 21
    BC6H_UF16 = 22; BC7_UNORM = 23; BC7_SRGB = 24
    ASTC_4x4_UNORM = 25; ASTC_4x4_SRGB = 26
    ASTC_6x6_UNORM = 27; ASTC_6x6_SRGB = 28
    ETC2_RGB8 = 29; ETC2_RGBA8 = 30
    PNG = 64; JPEG = 65; KTX2 = 66; DDS = 67


class TextureFlags(IntEnum):
    SRGB = 1 << 0
    HAS_MIPS = 1 << 1
    CUBEMAP = 1 << 2
    ARRAY = 1 << 3
    COMPRESSED = 1 << 4
    PREMULTIPLIED = 1 << 5
    GENERATE_MIPS = 1 << 6


class TextureRefFlags(IntEnum):
    KEEP_ALPHA = 1 << 0
    IGNORE_SRGB = 1 << 1


class TextureFilter(IntEnum):
    NEAREST = 0
    LINEAR = 1


class TextureMipFilter(IntEnum):
    NEAREST = 0
    LINEAR = 1


class TextureWrap(IntEnum):
    REPEAT = 0
    CLAMP_TO_EDGE = 1
    MIRRORED_REPEAT = 2
    CLAMP_TO_BORDER = 3


class TextureCompareFunc(IntEnum):
    NONE = 0; LESS = 1; LEQUAL = 2; GREATER = 3
    GEQUAL = 4; EQUAL = 5; NOT_EQUAL = 6; ALWAYS = 7


class AnimationPath(IntEnum):
    TRANSLATION = 0
    ROTATION = 1
    SCALE = 2


class AnimationInterpolation(IntEnum):
    LINEAR = 0
    STEP = 1
    CUBIC = 2


# ------------------------------------------------------------ struct 布局
FILE_HEADER_FMT   = '<IHHIIQQIII20s'   # 64
CHUNK_ENTRY_FMT   = '<IIQQQQII'        # 48
MODEL_META_FMT    = '<12Q6f12I'        # 168
VERTEX_STREAM_FMT = '<IIIIQQII6fII'    # 72
INDEX_BUFFER_FMT  = '<IIQQII'          # 32
SUBMESH_FMT       = '<6I6f2I'          # 56
TEXTURE_REF_FMT   = '<4I'              # 16
MATERIAL_FMT      = '<Q11f25I'         # 152
TEXTURE_DESC_FMT  = '<5Q12I'           # 88
TEXTURE_MIP_FMT   = '<QQII'            # 24
SAMPLER_DESC_FMT  = '<Q6I2f4I'         # 56
BONE_FMT          = '<2I10fI'          # 52
SKELETON_FMT      = '<2I3Q2I'          # 40
ANIM_CHANNEL_FMT  = '<4I2Q2I'          # 40
ANIMATION_FMT     = '<QfIQ2I'          # 32
LOD_DESC_FMT      = '<f3I'             # 16

for _n, _f, _e in [
    ('FileHeader',   FILE_HEADER_FMT,   64),
    ('ChunkEntry',   CHUNK_ENTRY_FMT,   48),
    ('ModelMeta',    MODEL_META_FMT,    168),
    ('VertexStream', VERTEX_STREAM_FMT, 72),
    ('IndexBuffer',  INDEX_BUFFER_FMT,  32),
    ('Submesh',      SUBMESH_FMT,       56),
    ('TextureRef',   TEXTURE_REF_FMT,   16),
    ('Material',     MATERIAL_FMT,      152),
    ('TextureDesc',  TEXTURE_DESC_FMT,  88),
    ('TextureMip',   TEXTURE_MIP_FMT,   24),
    ('SamplerDesc',  SAMPLER_DESC_FMT,  56),
    ('Bone',         BONE_FMT,          52),
    ('Skeleton',     SKELETON_FMT,      40),
    ('AnimChannel',  ANIM_CHANNEL_FMT,  40),
    ('Animation',    ANIMATION_FMT,     32),
    ('LodDesc',      LOD_DESC_FMT,      16),
]:
    _s = struct.calcsize(_f)
    assert _s == _e, '%s 尺寸错: %d != %d' % (_n, _s, _e)


# ------------------------------------------------------------ 通用工具
def align_up(x, a):
    return (x + a - 1) // a * a


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


# ------------------------------------------------------------ VAF 元数据
try:
    struct.pack('<e', 0.0)
    _HAS_F16 = True
except struct.error:
    _HAS_F16 = False

_VAF_INFO = {
    VAF.UNUSED:  None,
    VAF.FLOAT32: ('f', 4, None),
    VAF.FLOAT16: ('e', 2, None) if _HAS_F16 else None,
    VAF.SINT8:   ('b', 1, None),
    VAF.SNORM8:  ('b', 1, 127.0),
    VAF.UINT8:   ('B', 1, None),
    VAF.UNORM8:  ('B', 1, 255.0),
    VAF.SINT16:  ('h', 2, None),
    VAF.SNORM16: ('h', 2, 32767.0),
    VAF.UINT16:  ('H', 2, None),
    VAF.UNORM16: ('H', 2, 65535.0),
    VAF.OCT16_2: ('h', 2, 'oct'),
}

_ATTR_COMPONENTS = {
    0: 3,  # POSITION
    1: 3,  # NORMAL
    2: 4,  # TANGENT
    3: 2,  # TEXCOORD0
    4: 2,  # TEXCOORD1
    5: 4,  # COLOR
    6: 4,  # JOINTS
    7: 4,  # WEIGHTS
}

_SLOT_TO_KEY = {
    0: 'position', 1: 'normal', 2: 'tangent',
    3: 'uv0', 4: 'uv1', 5: 'color',
    6: 'joints', 7: 'weights',
}


def vaf_info(vaf):
    if vaf is None:
        return None
    try:
        return _VAF_INFO.get(VAF(vaf))
    except ValueError:
        return None


def attr_components(slot, vaf):
    if vaf == VAF.OCT16_2:
        return 2
    return _ATTR_COMPONENTS.get(slot, 0)


def attr_byte_size(slot, vaf):
    info = vaf_info(vaf)
    if info is None:
        return 0
    _, sz, _ = info
    return attr_components(slot, vaf) * sz


def unpack_all_vafs(format_word):
    """返回 {slot: vaf}，slot 0..7。"""
    return {s: (format_word >> (s * 4)) & 0xF for s in range(8)}


def pack_vafs(vafs):
    """{slot: vaf} → uint32。"""
    w = 0
    for s in range(8):
        v = int(vafs.get(s, 0)) & 0xF
        w |= v << (s * 4)
    return w


def stream_layout(attribute_mask, format_word, stride):
    """返回 [(slot, key, byte_offset, vaf, n_comp)]，按 slot 升序。"""
    vafs = unpack_all_vafs(format_word)
    layout = []
    off = 0
    for s in range(8):
        if not (attribute_mask & (1 << s)):
            continue
        vaf = vafs[s]
        if vaf == VAF.UNUSED:
            continue
        n = attr_components(s, vaf)
        layout.append((s, _SLOT_TO_KEY.get(s, '?'), off, vaf, n))
        off += attr_byte_size(s, vaf)
    if off > stride:
        raise ValueError('属性总字节 %d > stride %d' % (off, stride))
    return layout


# ------------------------------------------------------------ 纹理格式信息
# (blockWidth, blockHeight, blockBytes, bytesPerPixel, compressed)
_TEX_FMT_INFO = {
    TextureFormat.R8_UNORM:        (1, 1, 1, 1, False),
    TextureFormat.RG8_UNORM:       (1, 1, 2, 2, False),
    TextureFormat.RGBA8_UNORM:     (1, 1, 4, 4, False),
    TextureFormat.RGBA8_SRGB:      (1, 1, 4, 4, False),
    TextureFormat.BGRA8_UNORM:     (1, 1, 4, 4, False),
    TextureFormat.BGRA8_SRGB:      (1, 1, 4, 4, False),
    TextureFormat.RGBA16_FLOAT:    (1, 1, 8, 8, False),
    TextureFormat.RGBA32_FLOAT:    (1, 1, 16, 16, False),
    TextureFormat.BC1_UNORM:       (4, 4, 8, 0, True),
    TextureFormat.BC1_SRGB:        (4, 4, 8, 0, True),
    TextureFormat.BC3_UNORM:       (4, 4, 16, 0, True),
    TextureFormat.BC3_SRGB:        (4, 4, 16, 0, True),
    TextureFormat.BC4_UNORM:       (4, 4, 8, 0, True),
    TextureFormat.BC5_UNORM:       (4, 4, 16, 0, True),
    TextureFormat.BC6H_UF16:       (4, 4, 16, 0, True),
    TextureFormat.BC7_UNORM:       (4, 4, 16, 0, True),
    TextureFormat.BC7_SRGB:        (4, 4, 16, 0, True),
    TextureFormat.ASTC_4x4_UNORM:  (4, 4, 16, 0, True),
    TextureFormat.ASTC_4x4_SRGB:   (4, 4, 16, 0, True),
    TextureFormat.ASTC_6x6_UNORM:  (6, 6, 16, 0, True),
    TextureFormat.ASTC_6x6_SRGB:   (6, 6, 16, 0, True),
    TextureFormat.ETC2_RGB8:       (4, 4, 8, 0, True),
    TextureFormat.ETC2_RGBA8:      (4, 4, 16, 0, True),
}


def is_container_format(fmt):
    try:
        return TextureFormat(fmt) in (TextureFormat.PNG, TextureFormat.JPEG,
                                      TextureFormat.KTX2, TextureFormat.DDS)
    except ValueError:
        return False


def is_gpu_compressed_format(fmt):
    try:
        info = _TEX_FMT_INFO.get(TextureFormat(fmt))
    except ValueError:
        return False
    return info is not None and info[4]


def is_pixel_format(fmt):
    try:
        info = _TEX_FMT_INFO.get(TextureFormat(fmt))
    except ValueError:
        return False
    return info is not None and not info[4]


def compute_mip_slice_size(fmt, w, h):
    """单层单深度 mip 切片字节数。"""
    try:
        info = _TEX_FMT_INFO.get(TextureFormat(fmt))
    except ValueError:
        return 0
    if not info:
        return 0
    bw, bh, bb, bpp, comp = info
    if comp:
        bx = (w + bw - 1) // bw
        by = (h + bh - 1) // bh
        return bx * by * bb
    return w * h * bpp


def compute_mip_size(fmt, w, h, d, layers):
    return compute_mip_slice_size(fmt, w, h) * d * layers


def mip_width(base, level):
    v = base >> level
    return v if v > 0 else 1


def mip_height(base, level):
    v = base >> level
    return v if v > 0 else 1


def mip_depth(base, level):
    v = base >> level
    return v if v > 0 else 1


def compute_mip_offsets(fmt, width, height, depth, layers, mip_count):
    """返回 [(offset, size, w, h)]，offset 相对纹理数据起始。"""
    out = []
    off = 0
    for i in range(mip_count):
        w = mip_width(width, i)
        h = mip_height(height, i)
        d = mip_depth(depth, i)
        mip_size = compute_mip_size(fmt, w, h, d, layers)
        out.append((off, mip_size, w, h))
        off = align_up(off + mip_size, TEXTURE_DATA_ALIGNMENT)
    return out


def compute_pixel_data_size(fmt, width, height, depth, layers, mip_count):
    """像素/GPU 压缩格式，dataSize 应等于的值（末尾也对齐 256）。"""
    offsets = compute_mip_offsets(fmt, width, height, depth, layers, mip_count)
    if not offsets:
        return 0
    last_off, last_size, _, _ = offsets[-1]
    return align_up(last_off + last_size, TEXTURE_DATA_ALIGNMENT)


def is_valid_texture_shape(flags, width, height, depth, layers):
    if width == 0 or height == 0 or depth == 0 or layers == 0:
        return False
    cubemap = bool(flags & TextureFlags.CUBEMAP)
    is_array = bool(flags & TextureFlags.ARRAY)
    if cubemap:
        if depth != 1: return False
        if width != height: return False
        if layers % 6 != 0: return False
        if layers > 6 and not is_array: return False
    else:
        if depth > 1:
            if layers != 1: return False
            if is_array: return False
        else:
            if is_array != (layers > 1): return False
    return True


# ------------------------------------------------------------ 压缩
def decompress(codec, raw, expected=None):
    codec = CompressionCodec(codec)
    if codec == CompressionCodec.NONE:
        out = raw
    elif codec == CompressionCodec.DEFLATE:
        out = zlib.decompress(raw)
    elif codec == CompressionCodec.ZSTD:
        try:
            import zstandard
        except ImportError as e:
            raise NotImplementedError('需要 zstandard 模块') from e
        out = zstandard.ZstdDecompressor().decompress(raw)
    elif codec == CompressionCodec.LZ4:
        try:
            import lz4.frame
        except ImportError as e:
            raise NotImplementedError('需要 lz4 模块') from e
        out = lz4.frame.decompress(raw)
    else:
        raise NotImplementedError('未知压缩: %d' % codec)
    if expected is not None and len(out) != expected:
        raise ValueError('解压后长度 %d != 预期 %d' % (len(out), expected))
    return out


def compress(codec, raw):
    codec = CompressionCodec(codec)
    if codec == CompressionCodec.NONE:
        return raw
    if codec == CompressionCodec.DEFLATE:
        return zlib.compress(raw, 9)
    if codec == CompressionCodec.ZSTD:
        try:
            import zstandard
        except ImportError as e:
            raise NotImplementedError('需要 zstandard 模块') from e
        return zstandard.ZstdCompressor(level=19).compress(raw)
    if codec == CompressionCodec.LZ4:
        try:
            import lz4.frame
        except ImportError as e:
            raise NotImplementedError('需要 lz4 模块') from e
        return lz4.frame.compress(raw)
    raise NotImplementedError('未知压缩: %d' % codec)


# ------------------------------------------------------------ 八面体解码
def oct16_decode(x_raw, y_raw):
    x = x_raw / 32767.0
    y = y_raw / 32767.0
    z = 1.0 - abs(x) - abs(y)
    if z < 0.0:
        nx = (1.0 - abs(y)) * (1.0 if x >= 0 else -1.0)
        ny = (1.0 - abs(x)) * (1.0 if y >= 0 else -1.0)
        x, y = nx, ny
    ln = (x * x + y * y + z * z) ** 0.5
    if ln < 1e-12:
        return (0.0, 0.0, 1.0)
    return (x / ln, y / ln, z / ln)


def oct16_encode(x, y, z):
    """单位向量 → 2×snorm16。"""
    ln = (x * x + y * y + z * z) ** 0.5
    if ln < 1e-12:
        return (0, 32767)
    x /= ln; y /= ln; z /= ln
    # 投影到八面体
    l1 = abs(x) + abs(y) + abs(z)
    px = x / l1
    py = y / l1
    if z < 0.0:
        ox = (1.0 - abs(py)) * (1.0 if px >= 0 else -1.0)
        oy = (1.0 - abs(px)) * (1.0 if py >= 0 else -1.0)
        px, py = ox, oy
    qx = int(round(px * 32767.0))
    qy = int(round(py * 32767.0))
    qx = max(-32767, min(32767, qx))
    qy = max(-32767, min(32767, qy))
    return (qx, qy)
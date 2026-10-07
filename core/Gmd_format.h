#pragma once

#include <cstdint>
#include <cstddef>

#pragma pack(push, 1)

namespace gmd {

    // ============================================================================
    // 常量与魔数
    // ============================================================================

    // 魔数：内存字节 'G','M','D','B' -> 小端 uint32_t = 0x42444D47
    constexpr uint32_t kMagic = 0x42444D47;

    constexpr uint16_t kVersionMajor = 1;
    constexpr uint16_t kVersionMinor = 0;

    constexpr uint32_t kFileHeaderSize = 64;
    constexpr uint32_t kChunkEntrySize = 48;
    constexpr uint32_t kChunkAlignment = 64;

    // 纹理数据对齐
    constexpr uint32_t kTextureDataAlignment = 256;

    // 通用“无效”索引
    constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

    // FourCC 辅助函数
    constexpr uint32_t MakeFourCC(char a, char b, char c, char d) {
        return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
            (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
            (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
    }

    // 块类型 FourCC
    constexpr uint32_t CHUNK_META = MakeFourCC('M', 'E', 'T', 'A');
    constexpr uint32_t CHUNK_VRTX = MakeFourCC('V', 'R', 'T', 'X');
    constexpr uint32_t CHUNK_INDX = MakeFourCC('I', 'N', 'D', 'X');
    constexpr uint32_t CHUNK_SUBM = MakeFourCC('S', 'U', 'B', 'M');
    constexpr uint32_t CHUNK_MATL = MakeFourCC('M', 'A', 'T', 'L');
    constexpr uint32_t CHUNK_SKEL = MakeFourCC('S', 'K', 'E', 'L');
    constexpr uint32_t CHUNK_ANIM = MakeFourCC('A', 'N', 'I', 'M');
    constexpr uint32_t CHUNK_LODS = MakeFourCC('L', 'O', 'D', 'S');

    // 贴图相关块
    constexpr uint32_t CHUNK_TEXI = MakeFourCC('T', 'E', 'X', 'I'); // 纹理描述表
    constexpr uint32_t CHUNK_TEXD = MakeFourCC('T', 'E', 'X', 'D'); // 内嵌纹理数据
    constexpr uint32_t CHUNK_SMPL = MakeFourCC('S', 'M', 'P', 'L'); // 采样器表

    // ============================================================================
    // 枚举
    // ============================================================================

    // 文件标志
    enum FileFlags : uint32_t {
        FILE_FLAG_LITTLE_ENDIAN = 1u << 0,
        FILE_FLAG_COMPRESSED = 1u << 1,
        FILE_FLAG_EMBEDDED_TEX = 1u << 2, // 至少存在一条内嵌纹理（TEXD 存在）
        FILE_FLAG_QUANTIZED = 1u << 3,
        FILE_FLAG_UV_TOP_LEFT = 1u << 4,  // 若置位，UV 为 D3D 约定；否则为 OpenGL 约定
    };

    // 块标志
    enum ChunkFlags : uint32_t {
        CHUNK_FLAG_COMPRESSED = 1u << 0,
        CHUNK_FLAG_OPTIONAL = 1u << 1,
    };

    // 顶点属性位
    enum VertexAttribute : uint32_t {
        ATTR_POSITION = 1u << 0,
        ATTR_NORMAL = 1u << 1,
        ATTR_TANGENT = 1u << 2,
        ATTR_TEXCOORD0 = 1u << 3,
        ATTR_TEXCOORD1 = 1u << 4,
        ATTR_COLOR = 1u << 5,
        ATTR_JOINTS = 1u << 6,
        ATTR_WEIGHTS = 1u << 7,
    };

    // 索引类型
    enum IndexType : uint32_t {
        INDEX_TYPE_UINT16 = 0,
        INDEX_TYPE_UINT32 = 1,
    };

    // 量化标志
    enum QuantizationFlags : uint32_t {
        QUANT_POSITION_INT16 = 1u << 0,
        QUANT_NORMAL_OCT = 1u << 1,
        QUANT_TANGENT_OCT = 1u << 2,
        QUANT_UV_UINT16 = 1u << 3,
    };

    // 纹理来源
    enum TextureSource : uint32_t {
        TEX_SOURCE_EMBEDDED = 0, // 数据在 TEXD 块中
        TEX_SOURCE_EXTERNAL = 1, // 通过 URI 从外部加载
    };

    // 纹理格式
    enum TextureFormat : uint32_t {
        TEX_FORMAT_UNKNOWN = 0,

        // 未压缩像素
        TEX_FORMAT_R8_UNORM = 1,
        TEX_FORMAT_RG8_UNORM = 2,
        TEX_FORMAT_RGBA8_UNORM = 3,
        TEX_FORMAT_RGBA8_SRGB = 4,
        TEX_FORMAT_BGRA8_UNORM = 5,
        TEX_FORMAT_BGRA8_SRGB = 6,
        TEX_FORMAT_RGBA16_FLOAT = 7,
        TEX_FORMAT_RGBA32_FLOAT = 8,

        // GPU 压缩
        TEX_FORMAT_BC1_UNORM = 16,
        TEX_FORMAT_BC1_SRGB = 17,
        TEX_FORMAT_BC3_UNORM = 18,
        TEX_FORMAT_BC3_SRGB = 19,
        TEX_FORMAT_BC4_UNORM = 20,
        TEX_FORMAT_BC5_UNORM = 21,
        TEX_FORMAT_BC6H_UF16 = 22,
        TEX_FORMAT_BC7_UNORM = 23,
        TEX_FORMAT_BC7_SRGB = 24,
        TEX_FORMAT_ASTC_4x4_UNORM = 25,
        TEX_FORMAT_ASTC_4x4_SRGB = 26,
        TEX_FORMAT_ASTC_6x6_UNORM = 27,
        TEX_FORMAT_ASTC_6x6_SRGB = 28,
        TEX_FORMAT_ETC2_RGB8 = 29,
        TEX_FORMAT_ETC2_RGBA8 = 30,

        // 容器/编码图像（运行时解码）
        TEX_FORMAT_PNG = 64,
        TEX_FORMAT_JPEG = 65,
        TEX_FORMAT_KTX2 = 66,
        TEX_FORMAT_DDS = 67,
    };

    // 纹理标志
    enum TextureFlags : uint32_t {
        TEX_FLAG_SRGB = 1u << 0, // 颜色空间为 sRGB
        TEX_FLAG_HAS_MIPS = 1u << 1, // 含 mipmap
        TEX_FLAG_CUBEMAP = 1u << 2, // 立方体贴图
        TEX_FLAG_ARRAY = 1u << 3, // 纹理数组
        TEX_FLAG_COMPRESSED = 1u << 4, // TEXD 中该纹理数据被块级压缩（zstd/lz4）
        TEX_FLAG_PREMULTIPLIED = 1u << 5, // Alpha 预乘
        TEX_FLAG_GENERATE_MIPS = 1u << 6, // 运行时生成 mip
    };

    // 纹理引用标志
    enum TextureRefFlags : uint32_t {
        TEX_REF_FLAG_KEEP_ALPHA = 1u << 0,
        TEX_REF_FLAG_IGNORE_SRGB = 1u << 1,
    };

    // 采样器过滤
    enum TextureFilter : uint32_t {
        TEX_FILTER_NEAREST = 0,
        TEX_FILTER_LINEAR = 1,
    };

    enum TextureMipFilter : uint32_t {
        TEX_MIP_NEAREST = 0,
        TEX_MIP_LINEAR = 1,
    };

    enum TextureWrap : uint32_t {
        TEX_WRAP_REPEAT = 0,
        TEX_WRAP_CLAMP_TO_EDGE = 1,
        TEX_WRAP_MIRRORED_REPEAT = 2,
        TEX_WRAP_CLAMP_TO_BORDER = 3,
    };

    enum TextureCompareFunc : uint32_t {
        TEX_CMP_NONE = 0,
        TEX_CMP_LESS = 1,
        TEX_CMP_LEQUAL = 2,
        TEX_CMP_GREATER = 3,
        TEX_CMP_GEQUAL = 4,
        TEX_CMP_EQUAL = 5,
        TEX_CMP_NOT_EQUAL = 6,
        TEX_CMP_ALWAYS = 7,
    };

    // ============================================================================
    // 结构体定义
    // ============================================================================

    // 文件头（固定 64 字节）
    struct FileHeader {
        uint32_t magic;              // 0
        uint16_t versionMajor;       // 4
        uint16_t versionMinor;       // 6
        uint32_t flags;              // 8
        uint32_t headerSize;         // 12
        uint64_t fileSize;           // 16
        uint64_t chunkTableOffset;   // 24
        uint32_t chunkCount;         // 32
        uint32_t headerCRC32;        // 36
        uint8_t  reserved[24];       // 40
    };
    static_assert(sizeof(FileHeader) == kFileHeaderSize, "FileHeader size mismatch");

    // 块表项（固定 48 字节）
    struct ChunkEntry {
        uint32_t type;            // 0
        uint32_t flags;           // 4
        uint64_t offset;          // 8
        uint64_t size;            // 16
        uint64_t compressedSize;  // 24
        uint64_t alignment;       // 32
        uint32_t crc32;           // 40
        uint32_t reserved;        // 44
    };
    static_assert(sizeof(ChunkEntry) == kChunkEntrySize, "ChunkEntry size mismatch");

    // 模型元数据（META 块固定头部，共 168 字节）
    struct ModelMeta {
        uint64_t nameOffset;          // 0
        uint64_t vertexStreamsOffset; // 8
        uint64_t indexBuffersOffset;  // 16
        uint64_t submeshesOffset;     // 24
        uint64_t materialsOffset;     // 32
        uint64_t skeletonsOffset;     // 40
        uint64_t animationsOffset;    // 48
        uint64_t lodsOffset;          // 56
        uint64_t stringTableOffset;   // 64
        uint64_t stringTableSize;     // 72
        uint64_t texturesOffset;      // 80: 指向 TEXI 内 TextureDesc[]（相对 TEXI 块起始）
        uint64_t samplersOffset;      // 88: 指向 SMPL 内 SamplerDesc[]（相对 SMPL 块起始）
        float    boundsMin[3];        // 96
        float    boundsMax[3];        // 108
        uint32_t nameLength;          // 120
        uint32_t flags;               // 124
        uint32_t vertexStreamCount;   // 128
        uint32_t indexBufferCount;    // 132
        uint32_t submeshCount;        // 136
        uint32_t materialCount;       // 140
        uint32_t skeletonCount;       // 144
        uint32_t animationCount;      // 148
        uint32_t lodCount;            // 152
        uint32_t textureCount;        // 156
        uint32_t samplerCount;        // 160
        uint32_t reserved;            // 164
    };
    static_assert(sizeof(ModelMeta) == 168, "ModelMeta size mismatch");

    // 顶点流描述（VRTX 块内，共 72 字节）
    struct VertexStreamDesc {
        uint32_t streamIndex;         // 0
        uint32_t attributeMask;       // 4
        uint32_t vertexCount;         // 8
        uint32_t stride;              // 12
        uint64_t dataOffset;          // 16
        uint64_t dataSize;            // 24
        uint32_t format;              // 32
        uint32_t quantization;        // 36
        float    boundsMin[3];        // 40
        float    boundsMax[3];        // 52
        uint32_t reserved[2];         // 64
    };
    static_assert(sizeof(VertexStreamDesc) == 72, "VertexStreamDesc size mismatch");

    // 索引缓冲描述（INDX 块内，共 32 字节）
    struct IndexBufferDesc {
        uint32_t indexCount;   // 0
        uint32_t indexType;    // 4
        uint64_t dataOffset;   // 8
        uint64_t dataSize;     // 16
        uint32_t reserved[2];  // 24
    };
    static_assert(sizeof(IndexBufferDesc) == 32, "IndexBufferDesc size mismatch");

    // 子网格（SUBM 块内，共 56 字节）
    struct Submesh {
        uint32_t indexOffset;    // 0
        uint32_t indexCount;     // 4
        uint32_t vertexOffset;   // 8
        uint32_t materialIndex;  // 12
        uint32_t lodIndex;       // 16
        uint32_t flags;          // 20
        float    boundsMin[3];   // 24
        float    boundsMax[3];   // 36
        uint32_t reserved[2];    // 48
    };
    static_assert(sizeof(Submesh) == 56, "Submesh size mismatch");

    // 纹理引用（材质内使用，共 16 字节）
    struct TextureRef {
        uint32_t textureIndex; // 0: 指向 TextureDesc[] 的索引；kInvalidIndex 表示无
        uint32_t uvSet;        // 4: 使用的 UV 通道，0 表示 TEXCOORD0
        uint32_t samplerIndex; // 8: 覆盖采样器；kInvalidIndex 表示使用纹理默认采样器
        uint32_t flags;        // 12: TextureRefFlags
    };
    static_assert(sizeof(TextureRef) == 16, "TextureRef size mismatch");

    // 材质（MATL 块内，共 152 字节）
    struct Material {
        uint64_t   nameOffset;                // 0
        float      baseColorFactor[4];        // 8
        float      metallicFactor;            // 24
        float      roughnessFactor;           // 28
        float      emissiveFactor[3];         // 32
        float      normalScale;               // 44
        float      occlusionStrength;         // 48
        TextureRef baseColorTexture;          // 52
        TextureRef metallicRoughnessTexture;  // 68
        TextureRef normalTexture;             // 84
        TextureRef occlusionTexture;          // 100
        TextureRef emissiveTexture;           // 116
        uint32_t   flags;                     // 132
        uint32_t   customDataOffset;          // 136
        uint32_t   customDataSize;            // 140
        uint32_t   reserved[2];               // 144
    };
    static_assert(sizeof(Material) == 152, "Material size mismatch");

    // 纹理描述（TEXI 块内，共 88 字节）
    struct TextureDesc {
        uint64_t nameOffset;    // 0: StringTable 中名称偏移
        uint64_t uriOffset;     // 8: 外置时指向 StringTable 路径；0 表示内嵌
        uint64_t dataOffset;    // 16: 相对 TEXD 块起始的偏移（内嵌时有效）
        uint64_t dataSize;      // 24: 数据总字节数
        uint64_t mipsOffset;    // 32: 相对 TEXI 块起始，指向 TextureMipInfo[]；0 = 隐式推导
        uint32_t width;         // 40
        uint32_t height;        // 44
        uint32_t depth;         // 48: 3D 纹理深度，普通 2D 为 1
        uint32_t arrayLayers;   // 52: 数组层数，普通为 1
        uint32_t mipCount;      // 56: 至少为 1
        uint32_t format;        // 60: TextureFormat
        uint32_t samplerIndex;  // 64: 默认采样器索引，kInvalidIndex = 无
        uint32_t source;        // 68: TextureSource
        uint32_t flags;         // 72: TextureFlags
        uint32_t crc32;         // 76: dataSize 覆盖数据的 CRC32
        uint32_t reserved[2];   // 80
    };
    static_assert(sizeof(TextureDesc) == 88, "TextureDesc size mismatch");

    // 每个 mip 的信息（TEXI 块内，共 24 字节）
    struct TextureMipInfo {
        uint64_t offset;   // 0: 相对 TEXD 块起始
        uint64_t size;     // 8: 该 mip 字节数
        uint32_t width;    // 16
        uint32_t height;   // 20
    };
    static_assert(sizeof(TextureMipInfo) == 24, "TextureMipInfo size mismatch");

    // 采样器（SMPL 块内，共 56 字节）
    struct SamplerDesc {
        uint64_t nameOffset;    // 0: StringTable 中名称偏移
        uint32_t minFilter;     // 8: TextureFilter
        uint32_t magFilter;     // 12: TextureFilter
        uint32_t mipFilter;     // 16: TextureMipFilter
        uint32_t wrapS;         // 20: TextureWrap
        uint32_t wrapT;         // 24: TextureWrap
        uint32_t wrapR;         // 28: TextureWrap
        float    lodBias;       // 32
        float    maxAnisotropy; // 36
        uint32_t compareFunc;   // 40: TextureCompareFunc
        uint32_t flags;         // 44
        uint32_t reserved[2];   // 48
    };
    static_assert(sizeof(SamplerDesc) == 56, "SamplerDesc size mismatch");

    // 骨骼（SKEL 块内，共 52 字节）
    struct Bone {
        uint32_t parentIndex;         // 0
        uint32_t nameOffset;          // 4
        float    localTranslation[3]; // 8
        float    localRotation[4];    // 20
        float    localScale[3];       // 36
        uint32_t reserved;            // 48
    };
    static_assert(sizeof(Bone) == 52, "Bone size mismatch");

    // 骨骼集合（SKEL 块内，共 40 字节）
    struct Skeleton {
        uint32_t boneCount;                 // 0
        uint32_t rootBone;                  // 4
        uint64_t bonesOffset;               // 8
        uint64_t inverseBindMatricesOffset; // 16
        uint64_t nodeHierarchyOffset;       // 24
        uint32_t reserved[2];               // 32
    };
    static_assert(sizeof(Skeleton) == 40, "Skeleton size mismatch");

    // 动画通道（ANIM 块内，共 40 字节）
    struct AnimationChannel {
        uint32_t nodeIndex;      // 0
        uint32_t path;           // 4  (0=translation, 1=rotation, 2=scale)
        uint32_t interpolation;  // 8  (0=linear, 1=step, 2=cubic)
        uint32_t keyCount;       // 12
        uint64_t timesOffset;    // 16
        uint64_t valuesOffset;   // 24
        uint32_t reserved[2];    // 32
    };
    static_assert(sizeof(AnimationChannel) == 40, "AnimationChannel size mismatch");

    // 动画（ANIM 块内，共 32 字节）
    struct Animation {
        uint64_t nameOffset;      // 0
        float    duration;        // 8
        uint32_t channelCount;    // 12
        uint64_t channelsOffset;  // 16
        uint32_t reserved[2];     // 24
    };
    static_assert(sizeof(Animation) == 32, "Animation size mismatch");

    // LOD 描述（LODS 块内，共 16 字节）
    struct LodDesc {
        float    distance;        // 0
        uint32_t submeshStart;    // 4
        uint32_t submeshCount;    // 8
        uint32_t reserved;        // 12
    };
    static_assert(sizeof(LodDesc) == 16, "LodDesc size mismatch");

} // namespace gmd

#pragma pack(pop)
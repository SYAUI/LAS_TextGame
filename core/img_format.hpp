#pragma once

#include <cstdint>
#include <cstddef>

namespace img {

    // ============================================================================
    //  文件识别
    // ============================================================================
    constexpr char     FILE_EXTENSION[] = ".img";
    constexpr uint16_t VERSION_MAJOR = 1;
    constexpr uint16_t VERSION_MINOR = 0;
    constexpr uint32_t MAGIC =
        (static_cast<uint32_t>(VERSION_MAJOR) << 24) | 0x00A84D42u;

    // ============================================================================
    //  限制 / 对齐
    // ============================================================================
    constexpr uint32_t MAX_MIP_COUNT = 16;   // 支持到 65536x65536
    constexpr uint32_t MAX_ARRAY = 256;

    constexpr uint32_t ALIGN_ROW = 256;  // 行 pitch 对齐
    constexpr uint32_t ALIGN_SLICE = 512;  // mip / layer 起始对齐
    constexpr uint32_t ALIGN_PAYLOAD = 512;

    // ============================================================================
    //  像素格式
    // ============================================================================
    enum class PixelFormat : uint32_t {
        Unknown = 0,

        // ---- 未压缩 -----------------------------------------------------------
        R8_UNORM,
        R8G8_UNORM,
        R8G8B8A8_UNORM,
        R8G8B8A8_SRGB,
        B8G8R8A8_UNORM,
        B8G8R8A8_SRGB,
        R16_FLOAT,
        R16G16_FLOAT,
        R16G16B16A16_FLOAT,
        R32_FLOAT,
        R32G32B32A32_FLOAT,

        // ---- BC (DXT) ---------------------------------------------------------
        BC1_UNORM,
        BC1_SRGB,
        BC3_UNORM,
        BC3_SRGB,
        BC4_UNORM,
        BC5_UNORM,
        BC6H_UF16,
        BC6H_SF16,
        BC7_UNORM,
        BC7_SRGB,

        // ---- ASTC -------------------------------------------------------------
        ASTC_4x4_UNORM,
        ASTC_4x4_SRGB,
        ASTC_6x6_UNORM,
        ASTC_6x6_SRGB,
        ASTC_8x8_UNORM,
        ASTC_8x8_SRGB,

        // ---- ETC (移动) -------------------------------------------------------
        ETC2_RGB8_UNORM,
        ETC2_RGBA8_UNORM,
        EAC_R11_UNORM,
        EAC_RG11_UNORM,
    };

    struct FormatInfo {
        uint8_t blockWidth;    // 未压缩为 1
        uint8_t blockHeight;   // 未压缩为 1
        uint8_t bytesPerBlock; // 每块或每像素字节数
        bool    isSRGB;
        bool    isCompressed;
        bool    hasAlpha;
    };

    constexpr FormatInfo getFormatInfo(PixelFormat f) noexcept {
        switch (f) {
        case PixelFormat::R8_UNORM:           return { 1,1,1,  false,false,false };
        case PixelFormat::R8G8_UNORM:         return { 1,1,2,  false,false,false };
        case PixelFormat::R8G8B8A8_UNORM:     return { 1,1,4,  false,false,true };
        case PixelFormat::R8G8B8A8_SRGB:      return { 1,1,4,  true, false,true };
        case PixelFormat::B8G8R8A8_UNORM:     return { 1,1,4,  false,false,true };
        case PixelFormat::B8G8R8A8_SRGB:      return { 1,1,4,  true, false,true };
        case PixelFormat::R16_FLOAT:          return { 1,1,2,  false,false,false };
        case PixelFormat::R16G16_FLOAT:       return { 1,1,4,  false,false,false };
        case PixelFormat::R16G16B16A16_FLOAT: return { 1,1,8,  false,false,true };
        case PixelFormat::R32_FLOAT:          return { 1,1,4,  false,false,false };
        case PixelFormat::R32G32B32A32_FLOAT: return { 1,1,16, false,false,true };

        case PixelFormat::BC1_UNORM:  return { 4,4,8,  false,true,false };
        case PixelFormat::BC1_SRGB:   return { 4,4,8,  true, true,false };
        case PixelFormat::BC3_UNORM:  return { 4,4,16, false,true,true };
        case PixelFormat::BC3_SRGB:   return { 4,4,16, true, true,true };
        case PixelFormat::BC4_UNORM:  return { 4,4,8,  false,true,false };
        case PixelFormat::BC5_UNORM:  return { 4,4,16, false,true,false };
        case PixelFormat::BC6H_UF16:  return { 4,4,16, false,true,false };
        case PixelFormat::BC6H_SF16:  return { 4,4,16, false,true,false };
        case PixelFormat::BC7_UNORM:  return { 4,4,16, false,true,true };
        case PixelFormat::BC7_SRGB:   return { 4,4,16, true, true,true };

        case PixelFormat::ASTC_4x4_UNORM: return { 4,4,16, false,true,true };
        case PixelFormat::ASTC_4x4_SRGB:  return { 4,4,16, true, true,true };
        case PixelFormat::ASTC_6x6_UNORM: return { 6,6,16, false,true,true };
        case PixelFormat::ASTC_6x6_SRGB:  return { 6,6,16, true, true,true };
        case PixelFormat::ASTC_8x8_UNORM: return { 8,8,16, false,true,true };
        case PixelFormat::ASTC_8x8_SRGB:  return { 8,8,16, true, true,true };

        case PixelFormat::ETC2_RGB8_UNORM:  return { 4,4,8,  false,true,false };
        case PixelFormat::ETC2_RGBA8_UNORM: return { 4,4,16, false,true,true };
        case PixelFormat::EAC_R11_UNORM:    return { 4,4,8,  false,true,false };
        case PixelFormat::EAC_RG11_UNORM:   return { 4,4,16, false,true,false };

        default: return { 0,0,0,false,false,false };
        }
    }

    // ============================================================================
    //  头部标志位
    // ============================================================================
    enum HeaderFlags : uint16_t {
        HF_None = 0,
        HF_SRGB = 1u << 0,
        HF_Premultiplied = 1u << 1,
        HF_Cubemap = 1u << 2,
        HF_Volume = 1u << 3,
        HF_HasMetadata = 1u << 4,
        HF_Streamable = 1u << 5,   // 允许按 mip 流式加载
        HF_Mipmapped = 1u << 6,
        HF_CompressedData = 1u << 7,   // payload 外再包一层 LZ4/zstd（运行时解压）
        HF_SignedFormat = 1u << 8,
        HF_HDR = 1u << 9,
        HF_FlippedY = 1u << 10,  // 数据以 Y 翻转存储
        HF_AlphaOnly = 1u << 11,
        HF_User0 = 1u << 12,
        HF_User1 = 1u << 13,
        HF_User2 = 1u << 14,
        HF_User3 = 1u << 15,
    };

    // ============================================================================
    //  主头部
    //
    //  * 固定布局，所有多字节字段小端
    //  * mip 偏移 / 大小表内嵌，方便 mmap 后直接定位
    //  * 头之后可选跟 metadata 区域（由 metadataOffset / metadataSize 指向）
    // ============================================================================
#pragma pack(push, 1)

    struct alignas(8) Header {
        uint32_t magic;              // MAGIC
        uint16_t versionMajor;       // VERSION_MAJOR
        uint16_t versionMinor;       // VERSION_MINOR
        uint16_t headerSize;         // sizeof(Header) + mip 表
        uint16_t flags;              // HeaderFlags
        uint32_t format;             // PixelFormat
        uint16_t width;
        uint16_t height;
        uint16_t depth;              // 1 表示 2D
        uint16_t arraySize;          // layers * faces
        uint8_t  mipCount;           // 至少 1
        uint8_t  dimensions;         // 1 / 2 / 3
        uint16_t reserved0;
        uint32_t payloadOffset;      // 文件绝对偏移
        uint32_t payloadSize;
        uint32_t metadataOffset;     // 0 表示无元数据
        uint32_t metadataSize;
        uint32_t contentHashLo;      // 内容哈希（用于缓存失效 / 热重载）
        uint32_t contentHashHi;
        uint32_t mipOffsets[MAX_MIP_COUNT];  // 绝对偏移，0 表示未使用
        uint32_t mipSizes[MAX_MIP_COUNT];  // 字节数（未压缩时）
        uint32_t reserved2[8];
    };

#pragma pack(pop)

    // ============================================================================
    //  元数据（可选）
    // ============================================================================
    enum class MetadataKind : uint32_t {
        None = 0,
        SpriteAtlas = 1,   // 精灵 / 图标 / 九宫格
        FontAtlas = 2,   // 位图字体 / SDF
        Animation = 3,   // 帧动画
    };

    struct MetadataHeader {
        uint32_t kind;      // MetadataKind
        uint32_t size;      // 包含本头部的总大小
        uint32_t version;
        uint32_t reserved;
    };

    // 单个精灵矩形（相对 atlas 左上角，单位像素）
    struct Sprite {
        uint16_t x, y, w, h;
        int16_t  pivotX, pivotY;               // 锚点，可负
        uint16_t sliceL, sliceT, sliceR, sliceB; // 九宫格边框；全 0 表示普通矩形
        uint32_t nameOffset;                   // 相对元数据起始的名称字符串偏移（UTF-8, null-terminated）
        uint16_t flags;
        uint16_t reserved;
    };

    struct SpriteAtlasMetadata {
        MetadataHeader header;        // kind = SpriteAtlas
        uint32_t spriteCount;
        uint32_t nameTableOffset;     // 相对元数据起始
        uint32_t nameTableSize;
        uint32_t reserved;
        // 紧随其后：Sprite sprites[spriteCount]
        // 紧随其后：char nameTable[nameTableSize]
    };

    // ============================================================================
    //  工具函数
    // ============================================================================
    inline bool isValid(const Header& h) noexcept {
        if (h.magic != MAGIC) return false;
        if (h.headerSize < sizeof(Header)) return false;
        if (h.mipCount == 0 || h.mipCount > MAX_MIP_COUNT) return false;
        if (h.width == 0 || h.height == 0) return false;
        if (h.dimensions == 0 || h.dimensions > 3) return false;
        if (h.payloadOffset == 0 || h.payloadSize == 0) return false;
        return true;
    }

    inline bool isCubemap(const Header& h) noexcept {
        return (h.flags & HF_Cubemap) != 0;
    }

    inline bool isSRGB(const Header& h) noexcept {
        return (h.flags & HF_SRGB) != 0;
    }

    inline bool hasMetadata(const Header& h) noexcept {
        return (h.flags & HF_HasMetadata) != 0 && h.metadataSize >= sizeof(MetadataHeader);
    }

    // 计算某个 mip 级别的宽 / 高 / 深
    inline uint32_t mipWidth(const Header& h, uint32_t level) noexcept {
        uint32_t v = h.width >> level;
        return v ? v : 1u;
    }
    inline uint32_t mipHeight(const Header& h, uint32_t level) noexcept {
        uint32_t v = h.height >> level;
        return v ? v : 1u;
    }
    inline uint32_t mipDepth(const Header& h, uint32_t level) noexcept {
        uint32_t v = h.depth >> level;
        return v ? v : 1u;
    }

    // 由尺寸推出最大 mip 数
    constexpr uint32_t calcMipCount(uint32_t w, uint32_t h = 1, uint32_t d = 1) noexcept {
        uint32_t m = (w > h) ? w : h;
        if (d > m) m = d;
        uint32_t n = 1;
        while (m > 1) { m >>= 1; ++n; }
        return n;
    }

    // 计算某一 mip（单层）的字节数（不含对齐填充）
    inline uint64_t calcMipBytes(PixelFormat fmt,
        uint32_t w, uint32_t h, uint32_t d = 1) noexcept {
        const FormatInfo info = getFormatInfo(fmt);
        if (info.bytesPerBlock == 0) return 0;

        const uint32_t bw = (w + info.blockWidth - 1) / info.blockWidth;
        const uint32_t bh = (h + info.blockHeight - 1) / info.blockHeight;
        return static_cast<uint64_t>(bw) * bh * d * info.bytesPerBlock;
    }

    // 将 offset 向上对齐到 a 的倍数
    constexpr uint32_t alignUp(uint32_t v, uint32_t a) noexcept {
        return (v + (a - 1)) & ~(a - 1);
    }

    // 组合 64 位内容哈希
    inline uint64_t contentHash(const Header& h) noexcept {
        return (static_cast<uint64_t>(h.contentHashHi) << 32) | h.contentHashLo;
    }

    // 检查头部对 GPU 上传是否自洽（mip 大小是否匹配格式）
    inline bool validateMipSizes(const Header& h) noexcept {
        for (uint32_t i = 0; i < h.mipCount; ++i) {
            const uint64_t expect = calcMipBytes(
                static_cast<PixelFormat>(h.format),
                mipWidth(h, i), mipHeight(h, i), mipDepth(h, i))
                * (h.arraySize ? h.arraySize : 1);
            if (h.mipSizes[i] < expect) return false;
        }
        return true;
    }

} // namespace img
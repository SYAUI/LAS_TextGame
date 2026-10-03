#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <climits>

namespace img {

    // ============================================================================
    //  文件识别
    // ============================================================================
    constexpr char     FILE_EXTENSION[] = ".tex";
    constexpr uint16_t VERSION_MAJOR = 1;
    constexpr uint16_t VERSION_MINOR = 0;
    constexpr uint32_t MAGIC =
        (static_cast<uint32_t>(VERSION_MAJOR) << 24) | 0x00A84D42u;

    // ============================================================================
    //  限制 / 对齐
    // ============================================================================
    constexpr uint32_t MAX_MIP_COUNT = 16;
    constexpr uint32_t MAX_ARRAY = 256;

    constexpr uint32_t ALIGN_ROW = 256;
    constexpr uint32_t ALIGN_SLICE = 512;
    constexpr uint32_t ALIGN_PAYLOAD = 512;
    constexpr uint32_t ALIGN_EXTENSION = 512;

    // 默认压缩块大小：256 KiB
    constexpr uint32_t DEFAULT_COMPRESSION_BLOCK = 256 * 1024;

    // ============================================================================
    //  像素格式
    // ============================================================================
    enum class PixelFormat : uint32_t {
        Unknown = 0,
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
        ASTC_4x4_UNORM,
        ASTC_4x4_SRGB,
        ASTC_6x6_UNORM,
        ASTC_6x6_SRGB,
        ASTC_8x8_UNORM,
        ASTC_8x8_SRGB,
        ETC2_RGB8_UNORM,
        ETC2_RGBA8_UNORM,
        EAC_R11_UNORM,
        EAC_RG11_UNORM,
    };

    struct FormatInfo {
        uint8_t blockWidth;
        uint8_t blockHeight;
        uint8_t bytesPerBlock;
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
        HF_Streamable = 1u << 5,
        HF_Mipmapped = 1u << 6,
        HF_CompressedData = 1u << 7,
        HF_SignedFormat = 1u << 8,
        HF_HDR = 1u << 9,
        HF_FlippedY = 1u << 10,
        HF_AlphaOnly = 1u << 11,
        HF_User0 = 1u << 12,
        HF_User1 = 1u << 13,
        HF_User2 = 1u << 14,
        HF_User3 = 1u << 15,
    };

    // ============================================================================
    //  主头部（冻结 360B）
    // ============================================================================
#pragma pack(push, 8)
    struct alignas(8) Header {
        uint32_t magic;
        uint16_t versionMajor;
        uint16_t versionMinor;
        uint16_t headerSize;
        uint16_t flags;
        uint32_t format;
        uint16_t width;
        uint16_t height;
        uint16_t depth;
        uint16_t arraySize;
        uint8_t  mipCount;
        uint8_t  dimensions;
        uint16_t reserved0;
        uint32_t reserved1;
        uint64_t payloadOffset;
        uint64_t payloadSize;
        uint64_t metadataOffset;
        uint64_t metadataSize;
        uint32_t contentHashLo;
        uint32_t contentHashHi;
        uint64_t mipOffsets[MAX_MIP_COUNT];
        uint64_t mipSizes[MAX_MIP_COUNT];
        // 扩展区元信息
        uint64_t extensionsOffset;   // 绝对文件偏移；0 = 无扩展
        uint32_t extensionsCount;    // 扩展条目数
        uint32_t extensionsSize;     // 扩展区总字节数（含表头 + 载荷）
        uint32_t reserved2[4];       // 剩余保留
    };
#pragma pack(pop)

    // ── 布局冻结 ──
    static_assert(sizeof(Header) == 360, "Header must be 360 bytes");
    static_assert(alignof(Header) == 8, "Header must be 8-byte aligned");
    static_assert(std::is_standard_layout<Header>::value, "Header must be std-layout");
    static_assert(std::is_trivially_copyable<Header>::value, "Header must be trivially copyable");
    static_assert(offsetof(Header, payloadOffset) == 32, "");
    static_assert(offsetof(Header, mipOffsets) == 72, "");
    static_assert(offsetof(Header, mipSizes) == 200, "");
    static_assert(offsetof(Header, extensionsOffset) == 328, "");
    static_assert(offsetof(Header, extensionsCount) == 336, "");
    static_assert(offsetof(Header, extensionsSize) == 340, "");
    static_assert(offsetof(Header, reserved2) == 344, "");

    // ============================================================================
    //  扩展区（TLV）
    // ============================================================================
    enum class ExtensionType : uint32_t {
        None = 0,
        Compression = 1,
    };

    struct ExtensionEntry {
        uint32_t type;      // ExtensionType
        uint32_t version;   // 该扩展的版本
        uint64_t offset;    // 相对扩展区起始的偏移
        uint64_t size;      // 该扩展的总字节数
    };

    struct ExtensionTableHeader {
        uint32_t count;
        uint32_t reserved;
    };

    // ============================================================================
    //  压缩
    // ============================================================================
    enum class CompressionCodec : uint32_t {
        None = 0,
        XPRESS_HUFFMAN = 1,
        Raw = 2,   // 显式表示"未压缩块"占位
    };

    struct BlockEntry {
        uint64_t fileOffset;        // 该块数据在文件中的绝对偏移
        uint32_t compressedSize;    // 0 表示该块未压缩
        uint32_t uncompressedSize;  // 原始字节数
        uint32_t mipIndex;          // 该块属于哪个 mip
        uint32_t reserved;
    };

    struct CompressionDesc {
        uint32_t codec;         // CompressionCodec
        uint32_t blockSize;     // 每块未压缩最大字节数
        uint32_t blockCount;    // 块总数
        uint32_t reserved;
    };

    // ============================================================================
    //  元数据
    // ============================================================================
    enum class MetadataKind : uint32_t {
        None = 0,
        SpriteAtlas = 1,
        FontAtlas = 2,
        Animation = 3,
    };

    struct MetadataHeader {
        uint32_t kind;
        uint32_t size;
        uint32_t version;
        uint32_t reserved;
    };

    struct Sprite {
        uint16_t x, y, w, h;
        int16_t  pivotX, pivotY;
        uint16_t sliceL, sliceT, sliceR, sliceB;
        uint32_t nameOffset;
        uint16_t flags;
        uint16_t reserved;
    };

    struct SpriteAtlasMetadata {
        MetadataHeader header;
        uint32_t spriteCount;
        uint32_t nameTableOffset;
        uint32_t nameTableSize;
        uint32_t reserved;
    };

    // ============================================================================
    //  工具函数
    // ============================================================================
    inline bool isValid(const Header& h) noexcept {
        if (h.magic != MAGIC) return false;
        if (h.headerSize < sizeof(Header)) return false;
        if (h.mipCount == 0 || h.mipCount > MAX_MIP_COUNT) return false;
        if (h.width == 0 || h.height == 0) return false;
        if (h.arraySize == 0 || h.arraySize > MAX_ARRAY) return false;
        if (h.dimensions == 0 || h.dimensions > 3) return false;
        if (h.payloadOffset == 0 || h.payloadSize == 0) return false;
        if (getFormatInfo(static_cast<PixelFormat>(h.format)).bytesPerBlock == 0)
            return false;
        if (h.payloadOffset > UINT64_MAX - h.payloadSize) return false;
        if (h.extensionsOffset != 0 &&
            h.extensionsOffset > UINT64_MAX - h.extensionsSize) return false;
        return true;
    }

    inline bool isCubemap(const Header& h) noexcept {
        return (h.flags & HF_Cubemap) != 0;
    }
    inline bool isSRGB(const Header& h) noexcept {
        return (h.flags & HF_SRGB) != 0;
    }
    inline bool isCompressed(const Header& h) noexcept {
        return (h.flags & HF_CompressedData) != 0;
    }
    inline bool hasExtensions(const Header& h) noexcept {
        return h.extensionsOffset != 0 && h.extensionsCount > 0;
    }
    inline bool hasMetadata(const Header& h) noexcept {
        return (h.flags & HF_HasMetadata) != 0 && h.metadataSize >= sizeof(MetadataHeader);
    }

    inline uint32_t mipWidth(const Header& h, uint32_t level) noexcept {
        const uint32_t v = static_cast<uint32_t>(h.width) >> level;
        return v ? v : 1u;
    }
    inline uint32_t mipHeight(const Header& h, uint32_t level) noexcept {
        const uint32_t v = static_cast<uint32_t>(h.height) >> level;
        return v ? v : 1u;
    }
    inline uint32_t mipDepth(const Header& h, uint32_t level) noexcept {
        const uint32_t v = static_cast<uint32_t>(h.depth) >> level;
        return v ? v : 1u;
    }

    constexpr uint32_t calcMipCount(uint32_t w, uint32_t h = 1, uint32_t d = 1) noexcept {
        uint32_t m = (w > h) ? w : h;
        if (d > m) m = d;
        uint32_t n = 1;
        while (m > 1) { m >>= 1; ++n; }
        return n;
    }

    inline uint64_t calcMipBytes(PixelFormat fmt,
        uint32_t w, uint32_t h, uint32_t d = 1) noexcept {
        const FormatInfo info = getFormatInfo(fmt);
        if (info.bytesPerBlock == 0) return 0;
        const uint32_t bw = (w + info.blockWidth - 1) / info.blockWidth;
        const uint32_t bh = (h + info.blockHeight - 1) / info.blockHeight;
        return static_cast<uint64_t>(bw) * bh * d * info.bytesPerBlock;
    }

    constexpr uint32_t alignUp32(uint32_t v, uint32_t a) noexcept {
        return (v + (a - 1)) & ~(a - 1);
    }
    constexpr uint64_t alignUp64(uint64_t v, uint64_t a) noexcept {
        return (v + (a - 1)) & ~(a - 1);
    }

    inline uint64_t contentHash(const Header& h) noexcept {
        return (static_cast<uint64_t>(h.contentHashHi) << 32) | h.contentHashLo;
    }

    inline bool validateMipSizes(const Header& h) noexcept {
        const PixelFormat fmt = static_cast<PixelFormat>(h.format);
        if (getFormatInfo(fmt).bytesPerBlock == 0) return false;
        const uint64_t layers = h.arraySize ? h.arraySize : 1u;
        for (uint32_t i = 0; i < h.mipCount; ++i) {
            const uint64_t expect =
                calcMipBytes(fmt, mipWidth(h, i), mipHeight(h, i), mipDepth(h, i)) * layers;
            if (h.mipSizes[i] < expect) return false;
        }
        return true;
    }

} // namespace img
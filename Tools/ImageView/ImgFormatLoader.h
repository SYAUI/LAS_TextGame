#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string.h>
#include <vector>

#include "..\..\core\Img_format.hpp"
#include "rgbcx.h"

namespace img {

    // ============================================================
    //  加载结果
    // ============================================================
    struct TexImage {
        int         width = 0;
        int         height = 0;
        PixelFormat srcFormat = PixelFormat::Unknown;
        uint32_t    mipCount = 1;
        uint16_t    flags = 0;
        bool        wasCompressed = false;

        // 始终输出为 BGRA8 未预乘（stride = width * 4）
        std::vector<uint8_t> pixels;

        void clear() {
            width = 0;
            height = 0;
            srcFormat = PixelFormat::Unknown;
            mipCount = 1;
            flags = 0;
            wasCompressed = false;
            pixels.clear();
        }
    };
    // ============================================================
    //  Payload 视图
    //  未压缩时直接引用 fileData，压缩时指向 scratch
    // ============================================================
    struct PayloadView {
        const uint8_t* data = nullptr;
        size_t         size = 0;
    };
    // ============================================================
    //  扩展名判断（大小写无关）
    // ============================================================
    inline bool IsTexFile(const wchar_t* path) {
        if (!path) return false;
        const wchar_t* dot = wcsrchr(path, L'.');
        if (!dot || dot[1] == L'\0') return false;
        return _wcsicmp(dot, L".tex") == 0;
    }

    // ============================================================
    //  读整文件
    // ============================================================
    inline bool ReadWholeFile(const wchar_t* path, std::vector<uint8_t>& out) {
        out.clear();

        HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;

        constexpr int64_t kMaxFileSize = 8ll * 1024 * 1024 * 1024;   // 8 GiB
        LARGE_INTEGER sz = {};
        if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 ||
            sz.QuadPart > kMaxFileSize) {
            ::CloseHandle(h);
            return false;
        }

        out.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        BOOL  ok = ::ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
        ::CloseHandle(h);

        if (!ok || rd != (DWORD)out.size()) {
            out.clear();
            return false;
        }
        return true;
    }

    // ============================================================
    //  压缩 API（ntdll 动态加载，零链接依赖）
    // ============================================================
    namespace detail {

        using NtStatus = LONG;

        typedef NtStatus(NTAPI* PFN_RtlGetCompressionWorkSpaceSize)(USHORT, PULONG, PULONG);
        typedef NtStatus(NTAPI* PFN_RtlCompressBuffer)(
            USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG, PULONG, PVOID);
        typedef NtStatus(NTAPI* PFN_RtlDecompressBuffer)(
            USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG);
        typedef NtStatus(NTAPI* PFN_RtlDecompressBufferEx)(
            USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG, PVOID);

        constexpr USHORT   kCompressionFormatXpressHuff = 0x0004;   // COMPRESSION_FORMAT_XPRESS_HUFF
        constexpr USHORT   kCompressionEngineMaximum = 0x0001;   // COMPRESSION_ENGINE_MAXIMUM
        constexpr USHORT   kCompressionFormatAndEngine =
            kCompressionFormatXpressHuff | kCompressionEngineMaximum;

        constexpr ULONG    kXpressHuffChunkSize = 4096;     // 必须是 512/1024/2048/4096
        constexpr NtStatus kStatusSuccess = 0;

        struct RtlApi {
            PFN_RtlGetCompressionWorkSpaceSize GetWorkSpaceSize = nullptr;
            PFN_RtlCompressBuffer              Compress = nullptr;
            PFN_RtlDecompressBuffer            Decompress = nullptr;
            PFN_RtlDecompressBufferEx          DecompressEx = nullptr;
            bool                               available = false;

            RtlApi() {
                HMODULE h = ::GetModuleHandleW(L"ntdll.dll");
                if (!h) h = ::LoadLibraryW(L"ntdll.dll");
                if (!h) return;

                GetWorkSpaceSize = reinterpret_cast<PFN_RtlGetCompressionWorkSpaceSize>(
                    ::GetProcAddress(h, "RtlGetCompressionWorkSpaceSize"));
                Compress = reinterpret_cast<PFN_RtlCompressBuffer>(
                    ::GetProcAddress(h, "RtlCompressBuffer"));
                Decompress = reinterpret_cast<PFN_RtlDecompressBuffer>(
                    ::GetProcAddress(h, "RtlDecompressBuffer"));
                DecompressEx = reinterpret_cast<PFN_RtlDecompressBufferEx>(
                    ::GetProcAddress(h, "RtlDecompressBufferEx"));

                available = (GetWorkSpaceSize && Compress && Decompress);
            }
        };

        inline const RtlApi& GetRtlApi() {
            static RtlApi api;   // C++11 magic static，线程安全
            return api;
        }

    } // namespace detail

    // ============================================================
    //  压缩单块
    // ============================================================
    inline bool CompressBlock(const uint8_t* src, uint32_t srcSize,
        std::vector<uint8_t>& out)
    {
        const auto& api = detail::GetRtlApi();
        if (!api.available) return false;

        ULONG wsSize = 0, fragWsSize = 0;
        if (api.GetWorkSpaceSize(detail::kCompressionFormatAndEngine,
            &wsSize, &fragWsSize) != detail::kStatusSuccess)
            return false;

        std::vector<uint8_t> workspace(wsSize ? wsSize : 1);

        // Huffman 头开销 + 膨胀余量
        out.resize((size_t)srcSize + srcSize / 8 + 1024);

        ULONG finalSize = 0;
        const auto status = api.Compress(
            detail::kCompressionFormatAndEngine,
            const_cast<PUCHAR>(src), srcSize,
            out.data(), (ULONG)out.size(),
            detail::kXpressHuffChunkSize,
            &finalSize,
            workspace.data());

        if (status != detail::kStatusSuccess) { out.clear(); return false; }
        out.resize(finalSize);
        return true;
    }

    // ============================================================
    //  解压单块（优先用 RtlDecompressBufferEx，支持 XPRESS_HUFF）
    // ============================================================
    inline bool DecompressBlock(const uint8_t* src, uint32_t srcSize,
        uint8_t* dst, uint32_t dstCapacity,
        uint32_t* outWritten = nullptr)
    {
        const auto& api = detail::GetRtlApi();
        if (!api.available) return false;

        ULONG finalSize = 0;

        // ── 优先：RtlDecompressBufferEx ──
        if (api.DecompressEx) {
            ULONG wsSize = 0, fragWsSize = 0;
            if (api.GetWorkSpaceSize(detail::kCompressionFormatAndEngine,
                &wsSize, &fragWsSize) != detail::kStatusSuccess)
                return false;

            std::vector<uint8_t> workspace(wsSize ? wsSize : 1);

            const auto status = api.DecompressEx(
                detail::kCompressionFormatAndEngine,
                dst, dstCapacity,
                const_cast<PUCHAR>(src), srcSize,
                &finalSize,
                workspace.data());

            if (status != detail::kStatusSuccess) return false;
            if (outWritten) *outWritten = finalSize;
            return true;
        }

        // ── 回退：RtlDecompressBuffer（仅 LZNT1 / XPRESS 非 Huffman）──
        const auto status = api.Decompress(
            detail::kCompressionFormatAndEngine,
            dst, dstCapacity,
            const_cast<PUCHAR>(src), srcSize,
            &finalSize);

        if (status != detail::kStatusSuccess) return false;
        if (outWritten) *outWritten = finalSize;
        return true;
    }

    // ============================================================
    //  扩展区遍历
    // ============================================================
    inline const ExtensionEntry* FindExtension(
        const Header& h, const uint8_t* fileBase, size_t fileSize,
        ExtensionType type)
    {
        if (!hasExtensions(h)) return nullptr;
        if (h.extensionsOffset >= fileSize) return nullptr;
        if (h.extensionsOffset + h.extensionsSize > fileSize) return nullptr;
        if (h.extensionsSize < sizeof(ExtensionTableHeader)) return nullptr;

        const uint8_t* extBase = fileBase + (size_t)h.extensionsOffset;
        const ExtensionTableHeader* tbl =
            reinterpret_cast<const ExtensionTableHeader*>(extBase);

        if (tbl->count > 256) return nullptr;   // 防御
        if (sizeof(ExtensionTableHeader) + (size_t)tbl->count * sizeof(ExtensionEntry)
        > h.extensionsSize)
            return nullptr;

        const ExtensionEntry* entries = reinterpret_cast<const ExtensionEntry*>(
            extBase + sizeof(ExtensionTableHeader));

        for (uint32_t i = 0; i < tbl->count; ++i)
            if (entries[i].type == static_cast<uint32_t>(type))
                return &entries[i];
        return nullptr;
    }

    // ============================================================
    //  解压整个 payload（未压缩则直通拷贝）
    // ============================================================
    inline bool DecompressPayloadTo(
        const Header& h, const uint8_t* fileBase, size_t fileSize,
        std::vector<uint8_t>& out)
    {
        out.clear();

        // ── 未压缩：直通 ──
        if (!isCompressed(h)) {
            if (h.payloadOffset + h.payloadSize > fileSize) return false;
            out.assign(fileBase + (size_t)h.payloadOffset,
                fileBase + (size_t)h.payloadOffset + (size_t)h.payloadSize);
            return true;
        }

        // ── 压缩路径 ──
        const ExtensionEntry* ext = FindExtension(h, fileBase, fileSize,
            ExtensionType::Compression);
        if (!ext || ext->size < sizeof(CompressionDesc)) return false;

        const uint8_t* extBase = fileBase + (size_t)h.extensionsOffset;
        const uint8_t* descBase = extBase + (size_t)ext->offset;

        CompressionDesc desc = {};
        std::memcpy(&desc, descBase, sizeof(desc));

        if (desc.codec != static_cast<uint32_t>(CompressionCodec::XPRESS_HUFFMAN))
            return false;
        if (desc.blockCount == 0 || desc.blockCount > 65536) return false;

        const size_t blockTableSize =
            sizeof(CompressionDesc) + (size_t)desc.blockCount * sizeof(BlockEntry);
        if (ext->size < blockTableSize) return false;

        const BlockEntry* blocks = reinterpret_cast<const BlockEntry*>(
            descBase + sizeof(CompressionDesc));

        // 计算解压后总大小
        uint64_t total = 0;
        for (uint32_t i = 0; i < desc.blockCount; ++i) {
            if (blocks[i].uncompressedSize == 0) return false;
            total += blocks[i].uncompressedSize;
        }
        if (total == 0 || total > ((uint64_t)1 << 34)) return false;   // 16 GiB 上限

        out.resize((size_t)total);

        uint64_t dstOff = 0;
        for (uint32_t i = 0; i < desc.blockCount; ++i) {
            const BlockEntry& b = blocks[i];

            const uint64_t readSize = b.compressedSize ? b.compressedSize
                : b.uncompressedSize;
            if (b.fileOffset > fileSize || readSize > fileSize - b.fileOffset)
                return false;

            const uint8_t* srcData = fileBase + (size_t)b.fileOffset;

            if (b.compressedSize == 0) {
                std::memcpy(out.data() + dstOff, srcData, b.uncompressedSize);
            }
            else {
                uint32_t written = 0;
                if (!DecompressBlock(srcData, b.compressedSize,
                    out.data() + dstOff, b.uncompressedSize, &written))
                    return false;
                if (written != b.uncompressedSize) return false;
            }
            dstOff += b.uncompressedSize;
        }
        return true;
    }
    // ============================================================
    //  获取 payload 视图（不拷贝）
    //  未压缩 → 直接引用 fileBase 内的 payload 段
    //  压缩   → 解压到 scratch，视图指向 scratch
    // ============================================================
    inline bool GetPayloadView(
        const Header& h, const uint8_t* fileBase, size_t fileSize,
        std::vector<uint8_t>& scratch, PayloadView& out)
    {
        out.data = nullptr;
        out.size = 0;

        if (!isCompressed(h)) {
            if (h.payloadOffset > fileSize) return false;
            if (h.payloadSize > fileSize - h.payloadOffset) return false;
            out.data = fileBase + (size_t)h.payloadOffset;
            out.size = (size_t)h.payloadSize;
            return true;
        }

        if (!DecompressPayloadTo(h, fileBase, fileSize, scratch)) return false;
        out.data = scratch.data();
        out.size = scratch.size();
        return true;
    }
    // ============================================================
    //  内部：BC1 解码一块 → BGRA8
    // ============================================================
    namespace detail {

        // rgbcx 解码后是 RGBA，这里转成 BGRA 并写入目标图
        inline void BlitBCBlockRGBAtoBGRA(
            const uint8_t* blockRGBA,      // 16 像素 × 4 字节 RGBA
            uint8_t* dstBase, uint32_t dstStride,   // 目标图起始 + stride
            uint32_t dstW, uint32_t dstH,
            uint32_t blockX, uint32_t blockY)
        {
            for (int y = 0; y < 4; ++y) {
                const uint32_t py = blockY + (uint32_t)y;
                if (py >= dstH) break;

                for (int x = 0; x < 4; ++x) {
                    const uint32_t px = blockX + (uint32_t)x;
                    if (px >= dstW) break;

                    const uint8_t* sb = blockRGBA + (y * 4 + x) * 4;
                    uint8_t* db = dstBase + (size_t)py * dstStride + (size_t)px * 4;

                    db[0] = sb[2];   // B
                    db[1] = sb[1];   // G
                    db[2] = sb[0];   // R
                    db[3] = sb[3];   // A
                }
            }
        }

    } // namespace detail

    // ============================================================
    //  payload → BGRA8
    // ============================================================
    inline bool DecodePayloadToBGRA(const Header& h,
        const uint8_t* src, size_t srcSize,
        std::vector<uint8_t>& out)
    {
        if (!src || srcSize == 0) return false;
        if (h.depth != 1 || h.arraySize != 1) return false;

        const PixelFormat fmt = static_cast<PixelFormat>(h.format);
        const FormatInfo  fi = getFormatInfo(fmt);
        if (fi.bytesPerBlock == 0) return false;

        const size_t px = (size_t)h.width * h.height;
        if (px == 0) return false;

        out.resize(px * 4);
        uint8_t* dst = out.data();

        // ── 未压缩格式 ──
        if (!fi.isCompressed) {
            const uint64_t need = calcMipBytes(fmt, h.width, h.height, 1);
            if (need == 0 || need > srcSize) return false;

            switch (fmt) {
            case PixelFormat::R8G8B8A8_UNORM:
            case PixelFormat::R8G8B8A8_SRGB:
                for (size_t i = 0; i < px; ++i) {
                    dst[i * 4 + 0] = src[i * 4 + 2];
                    dst[i * 4 + 1] = src[i * 4 + 1];
                    dst[i * 4 + 2] = src[i * 4 + 0];
                    dst[i * 4 + 3] = src[i * 4 + 3];
                }
                return true;

            case PixelFormat::B8G8R8A8_UNORM:
            case PixelFormat::B8G8R8A8_SRGB:
                std::memcpy(dst, src, px * 4);
                return true;

            case PixelFormat::R8_UNORM:
                for (size_t i = 0; i < px; ++i) {
                    const uint8_t v = src[i];
                    dst[i * 4 + 0] = v; dst[i * 4 + 1] = v;
                    dst[i * 4 + 2] = v; dst[i * 4 + 3] = 255;
                }
                return true;

            case PixelFormat::R8G8_UNORM:
                for (size_t i = 0; i < px; ++i) {
                    dst[i * 4 + 0] = 0;
                    dst[i * 4 + 1] = src[i * 2 + 1];
                    dst[i * 4 + 2] = src[i * 2 + 0];
                    dst[i * 4 + 3] = 255;
                }
                return true;

            default:
                return false;   // 浮点 / 其他压缩格式暂不支持
            }
        }

        // ── 压缩格式：BC1 / BC3（其余暂不支持）──
        const uint32_t bw = (h.width + 3) / 4;
        const uint32_t bh = (h.height + 3) / 4;
        const uint32_t dstStride = h.width * 4;

        uint8_t blockRGBA[16 * 4];

        switch (fmt) {
        case PixelFormat::BC1_UNORM:
        case PixelFormat::BC1_SRGB:
        {
            const uint64_t need = (uint64_t)bw * bh * 8;
            if (need == 0 || need > srcSize) return false;

            for (uint32_t by = 0; by < bh; ++by) {
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    const uint8_t* s = src + ((size_t)by * bw + bx) * 8;
                    rgbcx::unpack_bc1(s, blockRGBA);
                    detail::BlitBCBlockRGBAtoBGRA(
                        blockRGBA, dst, dstStride,
                        h.width, h.height, bx * 4, by * 4);
                }
            }
            return true;
        }

        case PixelFormat::BC3_UNORM:
        case PixelFormat::BC3_SRGB:
        {
            const uint64_t need = (uint64_t)bw * bh * 16;
            if (need == 0 || need > srcSize) return false;

            for (uint32_t by = 0; by < bh; ++by) {
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    const uint8_t* s = src + ((size_t)by * bw + bx) * 16;
                    rgbcx::unpack_bc3(s, blockRGBA);
                    detail::BlitBCBlockRGBAtoBGRA(
                        blockRGBA, dst, dstStride,
                        h.width, h.height, bx * 4, by * 4);
                }
            }
            return true;
        }

        default:
            return false;
        }
    }

    // ============================================================
    //  主入口
    // ============================================================
    inline bool LoadTexFile(const wchar_t* path, TexImage& out) {
        out.clear();
        if (!path) return false;

        std::vector<uint8_t> fileData;
        if (!ReadWholeFile(path, fileData)) return false;
        if (fileData.size() < sizeof(Header)) return false;

        Header h = {};
        std::memcpy(&h, fileData.data(), sizeof(h));

        if (!isValid(h)) return false;
        if (h.payloadOffset >= fileData.size()) return false;

        // 未压缩路径不拷贝 payload
        std::vector<uint8_t> scratch;
        PayloadView view = {};
        if (!GetPayloadView(h, fileData.data(), fileData.size(), scratch, view))
            return false;

        if (!DecodePayloadToBGRA(h, view.data, view.size, out.pixels))
            return false;

        out.width = h.width;
        out.height = h.height;
        out.srcFormat = static_cast<PixelFormat>(h.format);
        out.mipCount = h.mipCount;
        out.flags = h.flags;
        out.wasCompressed = isCompressed(h);
        return true;
    }

} // namespace img
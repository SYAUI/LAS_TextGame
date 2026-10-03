#pragma once

#include "framework.h"
#include "..\..\core\Img_format.hpp"
#include "ImgFormatLoader.h"

#define RGBCX_IMPLEMENTATION
#include "rgbcx.h"

#include <commdlg.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace tex_export {

    //  导出格式选择
    enum class ExportFormat {
        Auto,       // 按 alpha 自动选 BC1 / BC3
        BC1,        // 强制 BC1
        BC3,        // 强制 BC3
        BGRA8,      // 未压缩
    };

    // ============================================================
    //  私有：确保 rgbcx 已初始化
    // ============================================================
    inline void EnsureRgbcxInit() {
        static bool inited = false;
        if (!inited) {
            rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
            inited = true;
        }
    }

    // ============================================================
    //  私有：Alpha 类型检测
    // ============================================================
    enum class AlphaClass { None, OneBit, Gradient };

    inline AlphaClass ClassifyAlpha(const uint8_t* rgba, size_t pixelCount) {
        bool seenZero = false, seenFull = false;
        for (size_t i = 0; i < pixelCount; ++i) {
            const uint8_t a = rgba[i * 4 + 3];
            if (a == 0)        seenZero = true;
            else if (a == 255) seenFull = true;
            else               return AlphaClass::Gradient;
        }
        if (seenZero) return AlphaClass::OneBit;
        return AlphaClass::None;
    }

    // ============================================================
    //  私有：用 rgbcx 编码 BC1
    // ============================================================
    inline bool EncodeBC1WithRgbcx(const uint8_t* rgba,
        uint32_t width, uint32_t height,
        std::vector<uint8_t>& out)
    {
        if (!rgba || width == 0 || height == 0) return false;
        EnsureRgbcxInit();

        const uint32_t bw = (width + 3) / 4;
        const uint32_t bh = (height + 3) / 4;
        out.resize((size_t)bw * bh * 8);

        uint8_t block[16 * 4];   // 4x4 像素 RGBA

        for (uint32_t by = 0; by < bh; ++by) {
            for (uint32_t bx = 0; bx < bw; ++bx) {
                // 取 4x4 块，越界用边缘像素填充
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        uint32_t px = bx * 4 + x;
                        uint32_t py = by * 4 + y;
                        if (px >= width)  px = width - 1;
                        if (py >= height) py = height - 1;

                        const uint8_t* s = rgba + ((size_t)py * width + px) * 4;
                        uint8_t* d = block + (y * 4 + x) * 4;
                        d[0] = s[0]; d[1] = s[1];
                        d[2] = s[2]; d[3] = s[3];
                    }
                }

                uint8_t* dst = out.data() + ((size_t)by * bw + bx) * 8;

                // level 10
                // allow_3color = true（BC1 允许 3 色块）
                // use_transparent_texels_for_black = false
                rgbcx::encode_bc1(
                    10,                     // level
                    dst,
                    block,
                    true,                   // allow_3color
                    false);                 // use_transparent_texels_for_black
            }
        }
        return true;
    }

    // ============================================================
    //  私有：用 rgbcx 编码 BC3
    // ============================================================
    inline bool EncodeBC3WithRgbcx(const uint8_t* rgba,
        uint32_t width, uint32_t height,
        std::vector<uint8_t>& out)
    {
        if (!rgba || width == 0 || height == 0) return false;
        EnsureRgbcxInit();

        const uint32_t bw = (width + 3) / 4;
        const uint32_t bh = (height + 3) / 4;
        out.resize((size_t)bw * bh * 16);

        uint8_t block[16 * 4];

        for (uint32_t by = 0; by < bh; ++by) {
            for (uint32_t bx = 0; bx < bw; ++bx) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        uint32_t px = bx * 4 + x;
                        uint32_t py = by * 4 + y;
                        if (px >= width)  px = width - 1;
                        if (py >= height) py = height - 1;

                        const uint8_t* s = rgba + ((size_t)py * width + px) * 4;
                        uint8_t* d = block + (y * 4 + x) * 4;
                        d[0] = s[0]; d[1] = s[1];
                        d[2] = s[2]; d[3] = s[3];
                    }
                }

                uint8_t* dst = out.data() + ((size_t)by * bw + bx) * 16;

                rgbcx::encode_bc3(10, dst, block);   // level 10
            }
        }
        return true;
    }

    // ============================================================
    //  私有：保存路径对话框
    // ============================================================
    inline bool AskSavePath(HWND hwndOwner,
        const wchar_t* suggestedName,
        std::wstring& outPath)
    {
        wchar_t buffer[MAX_PATH] = {};
        if (suggestedName && *suggestedName)
            wcsncpy_s(buffer, suggestedName, _TRUNCATE);
        else
            wcscpy_s(buffer, L"output.tex");

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwndOwner;
        ofn.lpstrFilter =
            L"Texture Files (*.tex)\0*.tex\0"
            L"All Files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = buffer;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"tex";
        ofn.Flags = OFN_OVERWRITEPROMPT
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;

        if (!::GetSaveFileNameW(&ofn)) return false;
        outPath = buffer;
        return true;
    }

    // ============================================================
    //  私有：从现有路径推导建议导出名
    // ============================================================
    inline std::wstring SuggestNameFrom(const CString& currentPath)
    {
        if (currentPath.IsEmpty()) return L"output.tex";

        CString base = currentPath;
        if (base.Left(6) == L"[TEX] ") base = base.Mid(6);

        int slash = base.ReverseFind(L'\\');
        if (slash < 0) slash = base.ReverseFind(L'/');
        if (slash >= 0) base = base.Mid(slash + 1);

        int dot = base.ReverseFind(L'.');
        if (dot > 0) base = base.Left(dot);

        if (base.IsEmpty()) return L"output.tex";
        return std::wstring((LPCWSTR)base) + L".tex";
    }

    // ============================================================
    //  私有：压缩整段 payload
    // ============================================================
    inline bool CompressPayloadBlocks(
        const std::vector<uint8_t>& raw,
        uint32_t blockSize,
        std::vector<uint8_t>& outCompressed,
        std::vector<img::BlockEntry>& outEntries)
    {
        outCompressed.clear();
        outEntries.clear();

        const size_t total = raw.size();
        if (total == 0) return false;

        const size_t blockCount = (total + blockSize - 1) / blockSize;
        outCompressed.reserve(total);

        for (size_t i = 0; i < blockCount; ++i) {
            const size_t   off = i * blockSize;
            const uint32_t len = (uint32_t)((total - off < blockSize)
                ? (total - off) : blockSize);

            std::vector<uint8_t> comp;
            img::BlockEntry entry = {};

            if (img::CompressBlock(raw.data() + off, len, comp) &&
                comp.size() < (size_t)len * 9 / 10)
            {
                entry.compressedSize = (uint32_t)comp.size();
                entry.uncompressedSize = len;
                outCompressed.insert(outCompressed.end(),
                    comp.begin(), comp.end());
            }
            else {
                entry.compressedSize = 0;
                entry.uncompressedSize = len;
                outCompressed.insert(outCompressed.end(),
                    raw.begin() + off,
                    raw.begin() + off + len);
            }
            outEntries.push_back(entry);
        }
        return true;
    }

    // ============================================================
    //  私有：写 .tex（核心）
    // ============================================================
    inline bool WriteTexFile(const wchar_t* path,
        const std::vector<uint8_t>& bgraPixels,
        int width, int height,
        bool useCompression,
        ExportFormat fmt)
    {
        if (!path || bgraPixels.empty() || width <= 0 || height <= 0)
            return false;
        if (width > 65535 || height > 65535) return false;

        const size_t px = (size_t)width * height;

        // ── BGRA → RGBA（rgbcx 输入需要 RGBA）──
        std::vector<uint8_t> rgba;
        const uint8_t* rgbaPtr = nullptr;

        if (fmt != ExportFormat::BGRA8) {
            rgba.resize(px * 4);
            for (size_t i = 0; i < px; ++i) {
                rgba[i * 4 + 0] = bgraPixels[i * 4 + 2];
                rgba[i * 4 + 1] = bgraPixels[i * 4 + 1];
                rgba[i * 4 + 2] = bgraPixels[i * 4 + 0];
                rgba[i * 4 + 3] = bgraPixels[i * 4 + 3];
            }
            rgbaPtr = rgba.data();
        }

        // ── Auto 模式：按 alpha 类型自动选择 ──
        if (fmt == ExportFormat::Auto) {
            const auto cls = ClassifyAlpha(rgbaPtr, px);
            fmt = (cls == AlphaClass::Gradient)
                ? ExportFormat::BC3
                : ExportFormat::BC1;
        }

        // ── 生成 payload ──
        std::vector<uint8_t> raw;
        img::PixelFormat outFormat;

        switch (fmt) {
        case ExportFormat::BC1:
            if (!EncodeBC1WithRgbcx(rgbaPtr, (uint32_t)width,
                (uint32_t)height, raw)) return false;
            outFormat = img::PixelFormat::BC1_UNORM;
            break;

        case ExportFormat::BC3:
            if (!EncodeBC3WithRgbcx(rgbaPtr, (uint32_t)width,
                (uint32_t)height, raw)) return false;
            outFormat = img::PixelFormat::BC3_UNORM;
            break;

        default:   // BGRA8
            raw = bgraPixels;
            outFormat = img::PixelFormat::B8G8R8A8_UNORM;
            break;
        }

        // ── 压缩（BC1/BC3 已经紧凑，不再叠 XPRESS）──
        std::vector<img::BlockEntry> blockEntries;
        std::vector<uint8_t> payload = raw;
        bool compressed = false;

        if (fmt == ExportFormat::BGRA8 && useCompression
            && img::detail::GetRtlApi().available)
        {
            const uint32_t blockSize = img::DEFAULT_COMPRESSION_BLOCK;
            if (CompressPayloadBlocks(raw, blockSize, payload, blockEntries)
                && !blockEntries.empty())
            {
                bool anyCompressed = false;
                for (const auto& e : blockEntries)
                    if (e.compressedSize != 0) { anyCompressed = true; break; }
                const double ratio =
                    (double)payload.size() / (double)raw.size();
                if (anyCompressed && ratio < 0.98)
                    compressed = true;
            }
            if (!compressed) payload = raw;
        }

        // ── 组装头 ──
        img::Header h = {};
        h.magic = img::MAGIC;
        h.versionMajor = img::VERSION_MAJOR;
        h.versionMinor = img::VERSION_MINOR;
        h.headerSize = (uint16_t)sizeof(img::Header);
        h.flags = compressed ? img::HF_CompressedData : img::HF_None;
        h.format = (uint32_t)outFormat;
        h.width = (uint16_t)width;
        h.height = (uint16_t)height;
        h.depth = 1;
        h.arraySize = 1;
        h.mipCount = 1;
        h.dimensions = 2;

        // ── 扩展区（仅压缩时启用）──
        std::vector<uint8_t> extBlob;
        if (compressed) {
            img::CompressionDesc cd = {};
            cd.codec = (uint32_t)img::CompressionCodec::XPRESS_HUFFMAN;
            cd.blockSize = img::DEFAULT_COMPRESSION_BLOCK;
            cd.blockCount = (uint32_t)blockEntries.size();

            const size_t descSize = sizeof(img::CompressionDesc)
                + blockEntries.size() * sizeof(img::BlockEntry);

            img::ExtensionTableHeader tbl = {};
            tbl.count = 1;

            const size_t extSize = sizeof(tbl)
                + sizeof(img::ExtensionEntry)
                + descSize;

            extBlob.resize(extSize, 0);
            uint8_t* p = extBlob.data();

            std::memcpy(p, &tbl, sizeof(tbl));  p += sizeof(tbl);

            img::ExtensionEntry ent = {};
            ent.type = (uint32_t)img::ExtensionType::Compression;
            ent.version = 1;
            ent.offset = sizeof(tbl) + sizeof(img::ExtensionEntry);
            ent.size = descSize;
            std::memcpy(p, &ent, sizeof(ent));  p += sizeof(ent);

            std::memcpy(p, &cd, sizeof(cd));    p += sizeof(cd);
            std::memcpy(p, blockEntries.data(),
                blockEntries.size() * sizeof(img::BlockEntry));

            h.extensionsCount = 1;
            h.extensionsSize = (uint32_t)extSize;
        }

        // ── 布局 ──
        const uint64_t extOff = img::alignUp64(sizeof(img::Header),
            img::ALIGN_EXTENSION);
        const uint64_t extEnd = extOff + extBlob.size();
        const uint64_t payloadOff = img::alignUp64(extEnd, img::ALIGN_PAYLOAD);

        h.payloadOffset = payloadOff;
        h.payloadSize = payload.size();
        h.mipOffsets[0] = compressed ? 0 : payloadOff;
        h.mipSizes[0] = raw.size();

        if (compressed) {
            h.extensionsOffset = extOff;

            uint64_t cursor = payloadOff;
            for (size_t i = 0; i < blockEntries.size(); ++i) {
                blockEntries[i].fileOffset = cursor;
                cursor += blockEntries[i].compressedSize
                    ? blockEntries[i].compressedSize
                    : blockEntries[i].uncompressedSize;
            }
            uint8_t* p2 = extBlob.data()
                + sizeof(img::ExtensionTableHeader)
                + sizeof(img::ExtensionEntry)
                + sizeof(img::CompressionDesc);
            std::memcpy(p2, blockEntries.data(),
                blockEntries.size() * sizeof(img::BlockEntry));
        }

        // ── 写盘 ──
        HANDLE hf = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) return false;

        auto writeAt = [&](const void* data, size_t size, uint64_t offset) -> bool {
            LARGE_INTEGER li = {};
            li.QuadPart = (LONGLONG)offset;
            if (!::SetFilePointerEx(hf, li, nullptr, FILE_BEGIN)) return false;
            DWORD written = 0;
            return ::WriteFile(hf, data, (DWORD)size, &written, nullptr)
                && written == (DWORD)size;
            };

        bool ok = true;
        ok = ok && writeAt(&h, sizeof(h), 0);
        if (ok && !extBlob.empty())
            ok = ok && writeAt(extBlob.data(), extBlob.size(), extOff);
        if (ok)
            ok = ok && writeAt(payload.data(), payload.size(), payloadOff);

        ::CloseHandle(hf);

        // ── 往返自检 ──
        if (ok) {
            img::TexImage check;
            if (!img::LoadTexFile(path, check)) {
                ::DeleteFileW(path);
                return false;
            }
            if (check.width != width || check.height != height) {
                ::DeleteFileW(path);
                return false;
            }
            if (fmt != ExportFormat::BGRA8 && fmt != ExportFormat::Auto) {
                // BC 格式：尺寸对就认为成功（有损编码，不比对像素）
            }
        }

        return ok;
    }

    // ============================================================
    //  对外 API
    // ============================================================
    inline bool PromptAndExport(HWND hwndOwner,
        const std::vector<uint8_t>& bgra,
        int width, int height,
        const CString& currentPath,
        bool pixelPerfect)
    {
        if (bgra.empty() || width <= 0 || height <= 0) {
            ::MessageBoxW(hwndOwner, L"没有可导出的图像。",
                L"导出为 .tex", MB_ICONINFORMATION | MB_OK);
            return false;
        }
        if (width > 65535 || height > 65535) {
            ::MessageBoxW(hwndOwner,
                L"图像尺寸超过 65535，.tex 格式暂不支持。",
                L"导出为 .tex", MB_ICONWARNING | MB_OK);
            return false;
        }

        ExportFormat fmt = ExportFormat::Auto;
        bool         compress = false;

        if (pixelPerfect) {
            // 像素完美模式：强制 BGRA8 未压缩，不做 BC 有损编码
            fmt = ExportFormat::BGRA8;

            const int c = ::MessageBoxW(hwndOwner,
                L"是否启用 XPRESS 无损压缩？\n\n"
                L"（像素图熵较高，压缩收益通常有限）",
                L"导出为 .tex", MB_YESNO | MB_ICONQUESTION);
            compress = (c == IDYES);
        }
        else {
            // 原有流程：让用户选择格式
            const int fmtChoice = ::MessageBoxW(hwndOwner,
                L"选择存储格式：\n\n"
                L"   是  - 自动（按 alpha 选 BC1/BC3，推荐）\n"
                L"   否  - 未压缩 BGRA8（体积大，适合编辑）\n"
                L"   取消 - 放弃导出",
                L"导出为 .tex", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (fmtChoice == IDCANCEL) return false;

            fmt = (fmtChoice == IDYES) ? ExportFormat::Auto
                : ExportFormat::BGRA8;

            if (fmt == ExportFormat::BGRA8) {
                const int c = ::MessageBoxW(hwndOwner,
                    L"是否启用 XPRESS 无损压缩？",
                    L"导出为 .tex", MB_YESNO | MB_ICONQUESTION);
                compress = (c == IDYES);
            }
        }

        // ── 保存路径 ──
        const std::wstring suggested = SuggestNameFrom(currentPath);
        std::wstring outPath;
        if (!AskSavePath(hwndOwner, suggested.c_str(), outPath))
            return false;

        // ── 写文件 ──
        if (!WriteTexFile(outPath.c_str(), bgra, width, height, compress, fmt)) {
            ::MessageBoxW(hwndOwner, L"写入 .tex 文件失败。",
                L"导出为 .tex", MB_ICONWARNING | MB_OK);
            return false;
        }

        // ── 汇总 ──
        const wchar_t* fmtName =
            (fmt == ExportFormat::Auto) ? L"Auto (BC1/BC3)" :
            (fmt == ExportFormat::BC1) ? L"BC1" :
            (fmt == ExportFormat::BC3) ? L"BC3" : L"BGRA8";

        CString msg;
        msg.Format(
            L"导出成功。\n\n"
            L"尺寸:    %d x %d\n"
            L"格式:    %s\n"
            L"压缩:    %s\n"
            L"路径:    %s",
            width, height, fmtName,
            compress ? L"是" : L"否",
            (LPCWSTR)outPath.c_str());
        ::MessageBoxW(hwndOwner, msg, L"导出为 .tex",
            MB_ICONINFORMATION | MB_OK);
        return true;
    }

} // namespace tex_export
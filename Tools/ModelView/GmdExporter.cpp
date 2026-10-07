#include "GmdExporter.h"

#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace gmd {

    namespace {

        // ---------- WIC 单例 ----------
        IWICImagingFactory* GetWIC()
        {
            static IWICImagingFactory* factory = nullptr;
            if (!factory)
            {
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&factory));
            }
            return factory;
        }

        // ---------- 从 SRV 读回 RGBA8 像素 ----------
        // 仅支持 R8G8B8A8_UNORM(_SRGB) 且非 MSAA 的 2D 纹理（我们的加载器创建的正是这种）
        bool ReadbackTextureRGBA8(ID3D11Device* device,
            ID3D11DeviceContext* ctx,
            ID3D11ShaderResourceView* srv,
            std::vector<uint8_t>& outPixels,
            uint32_t& outW, uint32_t& outH)
        {
            if (!device || !ctx || !srv) return false;

            ComPtr<ID3D11Resource> res;
            srv->GetResource(&res);
            if (!res) return false;

            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(res.As(&tex))) return false;

            D3D11_TEXTURE2D_DESC desc = {};
            tex->GetDesc(&desc);

            if (desc.SampleDesc.Count != 1) return false;
            if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
                desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                return false;

            // 创建暂存纹理
            D3D11_TEXTURE2D_DESC staging = desc;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.MiscFlags = 0;

            ComPtr<ID3D11Texture2D> stagingTex;
            if (FAILED(device->CreateTexture2D(&staging, nullptr, &stagingTex)))
                return false;

            ctx->CopyResource(stagingTex.Get(), tex.Get());

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(ctx->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                return false;

            outW = desc.Width;
            outH = desc.Height;
            outPixels.resize(static_cast<size_t>(outW) * outH * 4);

            const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
            for (uint32_t y = 0; y < outH; ++y)
            {
                std::memcpy(outPixels.data() + static_cast<size_t>(y) * outW * 4,
                    src + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(outW) * 4);
            }

            ctx->Unmap(stagingTex.Get(), 0);
            return true;
        }

        // ---------- 用 WIC 将 RGBA8 编码为 PNG ----------
        bool EncodePNG(const std::vector<uint8_t>& rgba,
            uint32_t w, uint32_t h,
            std::vector<uint8_t>& outPNG)
        {
            IWICImagingFactory* wic = GetWIC();
            if (!wic) return false;

            ComPtr<IWICBitmap> bitmap;
            if (FAILED(wic->CreateBitmapFromMemory(
                w, h,
                GUID_WICPixelFormat32bppRGBA,
                w * 4,
                static_cast<UINT>(rgba.size()),
                const_cast<BYTE*>(rgba.data()),
                &bitmap)))
                return false;

            // 用全局内存创建 IStream 作为输出目标
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, 0);
            if (!hMem) return false;

            ComPtr<IStream> outStream;
            if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &outStream)))
            {
                GlobalFree(hMem);
                return false;
            }

            ComPtr<IWICBitmapEncoder> encoder;
            if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
                return false;
            if (FAILED(encoder->Initialize(outStream.Get(), WICBitmapEncoderNoCache)))
                return false;

            ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2> props;
            if (FAILED(encoder->CreateNewFrame(&frame, &props)))
                return false;
            if (FAILED(frame->Initialize(props.Get())))
                return false;
            if (FAILED(frame->SetSize(w, h)))
                return false;

            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
            if (FAILED(frame->SetPixelFormat(&fmt)))
                return false;
            if (FAILED(frame->WriteSource(bitmap.Get(), nullptr)))
                return false;
            if (FAILED(frame->Commit()))
                return false;
            if (FAILED(encoder->Commit()))
                return false;

            // 从流中读回字节
            STATSTG stat = {};
            if (FAILED(outStream->Stat(&stat, STATFLAG_NONAME)))
                return false;

            ULARGE_INTEGER sz;
            sz.QuadPart = stat.cbSize.QuadPart;
            outPNG.resize(static_cast<size_t>(sz.LowPart) +
                (static_cast<size_t>(sz.HighPart) << 32));

            LARGE_INTEGER zero = {};
            if (FAILED(outStream->Seek(zero, STREAM_SEEK_SET, nullptr)))
                return false;

            ULONG read = 0;
            if (FAILED(outStream->Read(outPNG.data(),
                static_cast<ULONG>(outPNG.size()),
                &read)))
                return false;
            outPNG.resize(read);

            return true;
        }

    } // namespace

    // ============================================================
    //  主导出入口
    // ============================================================
    bool ExportToFile(const std::string& path,
        const GmdExportInput& input,
        std::string* err)
    {
        if (!input.vertexData || input.vertexCount == 0 ||
            !input.indices || input.indexCount == 0)
        {
            if (err) *err = "空网格：没有顶点或索引数据";
            return false;
        }

        GmdModel model;

        // -------------------- 顶点流 --------------------
        VertexStreamDesc vs = {};
        vs.streamIndex = 0;
        vs.attributeMask = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD0;
        vs.vertexCount = input.vertexCount;
        vs.stride = input.vertexStride;
        vs.dataOffset = 0;
        vs.dataSize = static_cast<uint64_t>(input.vertexCount) * input.vertexStride;
        vs.format = 0;
        vs.quantization = 0;

        const uint8_t* vbytes = static_cast<const uint8_t*>(input.vertexData);
        model.vertexData.resize(vs.dataSize);

        constexpr uint32_t kUvByteOffset = 24;

        const uint32_t stride = input.vertexStride;
        for (uint32_t i = 0; i < input.vertexCount; ++i)
        {
            const uint8_t* src = vbytes + static_cast<size_t>(i) * stride;
            uint8_t* dst = model.vertexData.data() + static_cast<size_t>(i) * stride;

            std::memcpy(dst, src, stride);

            if (stride >= kUvByteOffset + 8)
            {
                float u, v;
                std::memcpy(&u, dst + kUvByteOffset, 4);
                std::memcpy(&v, dst + kUvByteOffset + 4, 4);

                // D3D 约定(左上) → OpenGL 约定(左下)
                v = 1.0f - v;

                std::memcpy(dst + kUvByteOffset, &u, 4);
                std::memcpy(dst + kUvByteOffset + 4, &v, 4);
            }
        }


        // 计算包围盒（顶点前 12 字节必然是位置，与运行时 Vertex 布局一致）
        {
            float mn[3] = { FLT_MAX,  FLT_MAX,  FLT_MAX };
            float mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (uint32_t i = 0; i < input.vertexCount; ++i)
            {
                const float* p = reinterpret_cast<const float*>(
                    vbytes + static_cast<size_t>(i) * input.vertexStride);
                for (int k = 0; k < 3; ++k)
                {
                    mn[k] = (std::min)(mn[k], p[k]);
                    mx[k] = (std::max)(mx[k], p[k]);
                }
            }
            for (int k = 0; k < 3; ++k)
            {
                vs.boundsMin[k] = mn[k];
                vs.boundsMax[k] = mx[k];
                model.boundsMin[k] = mn[k];
                model.boundsMax[k] = mx[k];
            }
        }

        model.vertexStreams.push_back(vs);

        // -------------------- 索引 --------------------
        IndexBufferDesc ib = {};
        ib.indexCount = input.indexCount;
        ib.indexType = INDEX_TYPE_UINT32;
        ib.dataOffset = 0;
        ib.dataSize = static_cast<uint64_t>(input.indexCount) * sizeof(uint32_t);

        model.indexData.assign(
            reinterpret_cast<const uint8_t*>(input.indices),
            reinterpret_cast<const uint8_t*>(input.indices) + ib.dataSize);
        model.indexBuffers.push_back(ib);

        // -------------------- 子网格 --------------------
        for (uint32_t i = 0; i < input.submeshCount; ++i)
        {
            const auto& s = input.submeshes[i];
            Submesh sm = {};
            sm.indexOffset = s.indexOffset;
            sm.indexCount = s.indexCount;
            sm.vertexOffset = 0;
            sm.materialIndex = s.materialIndex;
            sm.lodIndex = 0;
            sm.flags = 0;
            model.submeshes.push_back(sm);
        }

        // -------------------- 纹理（读回 + PNG 编码） --------------------
        // texRemap[原索引] = 新索引，-1 表示该纹理导出失败被跳过
        std::vector<int> texRemap(input.textureCount, -1);
        size_t newTexCount = 0;
        size_t texSkipped = 0;

        if (input.device && input.textureSRVs && input.textureCount > 0)
        {
            ID3D11DeviceContext* ctx = nullptr;
            input.device->GetImmediateContext(&ctx);
            if (ctx)
            {
                for (uint32_t i = 0; i < input.textureCount; ++i)
                {
                    ID3D11ShaderResourceView* srv = input.textureSRVs[i];
                    if (!srv) { texSkipped++; continue; }

                    std::vector<uint8_t> rgba;
                    uint32_t w = 0, h = 0;
                    if (!ReadbackTextureRGBA8(input.device, ctx, srv, rgba, w, h))
                    {
                        texSkipped++;
                        continue;
                    }

                    std::vector<uint8_t> png;
                    if (!EncodePNG(rgba, w, h, png))
                    {
                        texSkipped++;
                        continue;
                    }

                    TextureData td;
                    td.desc.source = TEX_SOURCE_EMBEDDED;
                    td.desc.format = TEX_FORMAT_PNG;
                    td.desc.width = w;
                    td.desc.height = h;
                    td.desc.depth = 1;
                    td.desc.arrayLayers = 1;
                    td.desc.mipCount = 1;
                    td.desc.samplerIndex = kInvalidIndex;
                    td.desc.flags = TEX_FLAG_SRGB;
                    td.desc.crc32 = 0;   // 由 GmdWriter 计算

                    td.name = (input.textureNames && input.textureNames[i])
                        ? input.textureNames[i]
                        : ("texture_" + std::to_string(i));

                    td.embeddedData = std::move(png);

                    texRemap[i] = static_cast<int>(newTexCount++);
                    model.textures.push_back(std::move(td));
                }
                ctx->Release();
            }
        }

        // -------------------- 材质 --------------------
        for (uint32_t i = 0; i < input.materialCount; ++i)
        {
            const auto& m = input.materials[i];
            MaterialData md;
            md.name = m.name ? m.name : ("material_" + std::to_string(i));

            md.mat.baseColorFactor[0] = m.baseColor[0];
            md.mat.baseColorFactor[1] = m.baseColor[1];
            md.mat.baseColorFactor[2] = m.baseColor[2];
            md.mat.baseColorFactor[3] = m.baseColor[3];

            md.mat.metallicFactor = 0.0f;
            md.mat.roughnessFactor = 1.0f;
            md.mat.emissiveFactor[0] = 0.0f;
            md.mat.emissiveFactor[1] = 0.0f;
            md.mat.emissiveFactor[2] = 0.0f;
            md.mat.normalScale = 1.0f;
            md.mat.occlusionStrength = 1.0f;

            auto clearRef = [](TextureRef& r) {
                r.textureIndex = kInvalidIndex;
                r.uvSet = 0;
                r.samplerIndex = kInvalidIndex;
                r.flags = 0;
                };
            clearRef(md.mat.baseColorTexture);
            clearRef(md.mat.metallicRoughnessTexture);
            clearRef(md.mat.normalTexture);
            clearRef(md.mat.occlusionTexture);
            clearRef(md.mat.emissiveTexture);

            if (m.textureIndex >= 0 &&
                m.textureIndex < static_cast<int>(input.textureCount))
            {
                int newIdx = texRemap[m.textureIndex];
                if (newIdx >= 0)
                    md.mat.baseColorTexture.textureIndex = static_cast<uint32_t>(newIdx);
            }

            model.materials.push_back(std::move(md));
        }

        // 若没有任何材质，塞一个默认的，避免下游空数组
        if (model.materials.empty())
        {
            MaterialData md;
            md.name = "default";
            md.mat.baseColorFactor[0] = 1.0f;
            md.mat.baseColorFactor[1] = 1.0f;
            md.mat.baseColorFactor[2] = 1.0f;
            md.mat.baseColorFactor[3] = 1.0f;
            md.mat.baseColorTexture.textureIndex = kInvalidIndex;
            md.mat.metallicRoughnessTexture.textureIndex = kInvalidIndex;
            md.mat.normalTexture.textureIndex = kInvalidIndex;
            md.mat.occlusionTexture.textureIndex = kInvalidIndex;
            md.mat.emissiveTexture.textureIndex = kInvalidIndex;
            model.materials.push_back(std::move(md));
        }

        // -------------------- 文件标志 --------------------
        model.flags = FILE_FLAG_LITTLE_ENDIAN;
        if (!model.textures.empty())
            model.flags |= FILE_FLAG_EMBEDDED_TEX;

        model.name = "Exported";

        // -------------------- 保存 --------------------
        return GmdWriter::Save(path, model, err);
    }

} // namespace gmd
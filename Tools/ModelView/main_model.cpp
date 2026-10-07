#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "ufbx.h"

#include "GmdExporter.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ============================================================
//  1. 着色器（含纹理）
// ============================================================
static const char* g_shaderSource = R"(
cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float4   gCameraPos;
    float4   gLightDir;
};

cbuffer PerObject : register(b1)
{
    float4x4 gWorld;
    float4   gColor;
    float    gUseTexture;
    float    gWireframe;
    float3   gPad;
};

Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct PSInput
{
    float4 pos      : SV_POSITION;
    float3 worldPos : POSITION0;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    float4 worldPos = mul(float4(input.pos, 1.0f), gWorld);
    o.worldPos = worldPos.xyz;
    o.pos      = mul(worldPos, gViewProj);
    o.normal   = normalize(mul(input.normal, (float3x3)gWorld));
    o.uv       = input.uv;
    return o;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // 线框模式：直接输出纯色，不参与光照/纹理
    if (gWireframe > 0.5f)
        return gColor;
    float3 N = normalize(input.normal);
    float3 L = normalize(-gLightDir.xyz);
    float3 V = normalize(gCameraPos.xyz - input.worldPos);
    float3 H = normalize(L + V);

    float diff = max(dot(N, L), 0.0f);
    float spec = pow(max(dot(N, H), 0.0f), 32.0f);

    float3 skyColor    = float3(0.35f, 0.42f, 0.55f);
    float3 groundColor = float3(0.15f, 0.13f, 0.10f);
    float  hemiMix     = saturate(N.y * 0.5f + 0.5f);
    float3 ambient     = lerp(groundColor, skyColor, hemiMix) * 0.5f;

    // 无贴图时 albedo 就是 gColor.rgb (纯色)；有贴图时二者相乘
    float3 albedo = gColor.rgb;
    if (gUseTexture > 0.5f)
        albedo *= gTexture.Sample(gSampler, input.uv).rgb;

    float3 color = albedo * (ambient + diff) + spec * 0.6f;
    return float4(color, gColor.a);
}
)";

// ============================================================
//  2. 数据结构
// ============================================================
struct Vertex
{
    XMFLOAT3 pos;
    XMFLOAT3 normal;
    XMFLOAT2 uv;
};

struct PerFrameCB
{
    XMFLOAT4X4 viewProj;
    XMFLOAT4   cameraPos;
    XMFLOAT4   lightDir;
};

struct PerObjectCB
{
    XMFLOAT4X4 world;
    XMFLOAT4   color;
    float      useTexture;
    float      wireframe;
    XMFLOAT2   pad2;
};

static_assert(sizeof(PerFrameCB) % 16 == 0, "PerFrameCB must be 16-byte aligned");
static_assert(sizeof(PerObjectCB) % 16 == 0, "PerObjectCB must be 16-byte aligned");

struct Camera
{
    XMFLOAT3 target = { 0, 0, 0 };
    float    distance = 5.0f;
    float    yaw = 0.5f;
    float    pitch = 0.35f;
    float    fovY = 45.0f;
    float    nearZ = 0.05f;
    float    farZ = 1000.0f;

    XMFLOAT3 GetPosition() const
    {
        float cp = cosf(pitch);
        return XMFLOAT3(
            target.x + distance * cp * sinf(yaw),
            target.y + distance * sinf(pitch),
            target.z + distance * cp * cosf(yaw));
    }

    XMMATRIX GetView() const
    {
        XMFLOAT3 eye = GetPosition();
        return XMMatrixLookAtLH(XMLoadFloat3(&eye),
            XMLoadFloat3(&target),
            XMVectorSet(0, 1, 0, 0));
    }

    XMMATRIX GetProj(float aspect) const
    {
        return XMMatrixPerspectiveFovLH(
            XMConvertToRadians(fovY), aspect, nearZ, farZ);
    }

    void Clamp()
    {
        const float kMaxPitch = XM_PIDIV2 - 0.05f;
        pitch = std::max(-kMaxPitch, std::min(kMaxPitch, pitch));
        distance = std::max(0.1f, std::min(500.0f, distance));
    }
};

// 材质回退原因（用于 UI 展示）
enum class MaterialFallbackReason
{
    None,           // 有贴图，正常
    NoTexture,      // FBX 里没定义基础色贴图
    LoadFailed,     // 定义了贴图，但加载失败
    NoBaseColor,    // 连 base_color 都没定义（罕见）
};

struct MaterialGPU
{
    XMFLOAT4    baseColor = { 1, 1, 1, 1 };  // 无贴图时使用的纯色
    int         textureIndex = -1;           // < 0 表示没有可用纹理
    std::string name;

    MaterialFallbackReason reason = MaterialFallbackReason::None;
    std::string            missingTexturePath; // 加载失败时记录路径
};

struct DrawCall
{
    UINT indexStart = 0;
    UINT indexCount = 0;
    int  materialIdx = 0;
};

// ============================================================
//  3. Renderer 全局状态
// ============================================================
struct Renderer
{
    HWND  hwnd = nullptr;
    int   width = 0;
    int   height = 0;

    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<IDXGISwapChain>         swapChain;
    ComPtr<ID3D11RenderTargetView> backRTV;
    ComPtr<ID3D11Texture2D>        backDepthTex;
    ComPtr<ID3D11DepthStencilView> backDSV;

    ComPtr<ID3D11Texture2D>          offColorTex;
    ComPtr<ID3D11RenderTargetView>   offRTV;
    ComPtr<ID3D11ShaderResourceView> offSRV;
    ComPtr<ID3D11Texture2D>          offDepthTex;
    ComPtr<ID3D11DepthStencilView>   offDSV;
    int offW = 0, offH = 0;

    ComPtr<ID3D11VertexShader>      vs;
    ComPtr<ID3D11PixelShader>       ps;
    ComPtr<ID3D11InputLayout>       inputLayout;
    ComPtr<ID3D11Buffer>            cbPerFrame;
    ComPtr<ID3D11Buffer>            cbPerObject;
    ComPtr<ID3D11RasterizerState>   rsSolid;
    ComPtr<ID3D11RasterizerState>   rsWire;
    ComPtr<ID3D11DepthStencilState> dsState;
    ComPtr<ID3D11SamplerState>      sampler;

    ComPtr<ID3D11Buffer> vb;
    ComPtr<ID3D11Buffer> ib;
    UINT indexCount = 0;

    std::vector<DrawCall>                          drawCalls;
    std::vector<MaterialGPU>                       materials;
    std::vector<ComPtr<ID3D11ShaderResourceView>>  textures;

    // 用于导出：保留一份 CPU 端的顶点/索引副本
    std::vector<Vertex>   cpuVertices;
    std::vector<uint32_t> cpuIndices;

    Camera camera;
    float  modelRotationY = 0.0f;
    bool   autoRotate = true;
    float  lightYaw = 0.6f;
    float  lightPitch = 0.8f;
    bool   showPanel = true;
    bool   wireframe = false;

    // 线框线条颜色（默认黑）
    XMFLOAT4 wireframeColor = { 0.0f, 0.0f, 0.0f, 1.0f };

    // 纯色回退配置
    bool     forceSolidColor = false;   // 调试开关：所有材质强制用纯色
    bool     useHashBasedFallback = true;    // 无 base_color 时按名字 hash 派生颜色
    XMFLOAT4 defaultFallbackColor = { 0.80f, 0.60f, 0.40f, 1.0f };

    // 状态栏
    std::string statusText;
    bool        statusIsError = false;

    std::wstring pendingFile;
    bool         hasPendingFile = false;
};

static Renderer g_r;
static bool     g_running = true;

// ============================================================
//  4. 工具函数
// ============================================================
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
        nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
        s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring GetDirectory(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? L"" : path.substr(0, p + 1);
}

// ufbx_string -> std::string（注意可能含 '\0'）
static std::string UfbxStr(const ufbx_string& s)
{
    if (!s.data || s.length == 0) return {};
    return std::string(s.data, s.length);
}

// 16 进制 FNV-1a，用于生成稳定的回退色
static uint32_t HashString(const char* s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static void DebugLogUfbxString(const char* prefix, const ufbx_string& s)
{
    if (!s.data || s.length == 0) return;
    std::string tmp;
    tmp.reserve(strlen(prefix) + s.length + 2);
    tmp += prefix;
    tmp.append(s.data, s.length);
    tmp += '\n';
    OutputDebugStringA(tmp.c_str());
}

// ============================================================
//  5. 纹理加载 (WIC)
// ============================================================
static IWICImagingFactory* g_wicFactory = nullptr;

static bool InitWIC()
{
    if (g_wicFactory) return true;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&g_wicFactory));
    return SUCCEEDED(hr);
}

static ComPtr<ID3D11ShaderResourceView> CreateSRVFromWICFrame(
    IWICBitmapFrameDecode* frame, ID3D11Device* device)
{
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(g_wicFactory->CreateFormatConverter(&converter))) return nullptr;
    if (FAILED(converter->Initialize(frame,
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom)))
        return nullptr;

    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0)
        return nullptr;

    std::vector<uint8_t> pixels((size_t)w * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4,
        (UINT)pixels.size(), pixels.data())))
        return nullptr;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = w * 4;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &sd, &tex))) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = td.Format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), &srvd, &srv)))
        return nullptr;
    return srv;
}

static ComPtr<ID3D11ShaderResourceView> LoadTextureFromMemory(
    const void* data, size_t size, ID3D11Device* device)
{
    if (!InitWIC() || !data || size == 0) return nullptr;
    if (size > 0xFFFFFFFFull) return nullptr; // WIC 限制为 DWORD

    ComPtr<IWICStream> stream;
    if (FAILED(g_wicFactory->CreateStream(&stream))) return nullptr;
    if (FAILED(stream->InitializeFromMemory((BYTE*)data, (DWORD)size)))
        return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(g_wicFactory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
        return nullptr;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;

    return CreateSRVFromWICFrame(frame.Get(), device);
}

static ComPtr<ID3D11ShaderResourceView> LoadTextureFromFile(
    const std::wstring& path, ID3D11Device* device)
{
    if (!InitWIC() || path.empty()) return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(g_wicFactory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder)))
        return nullptr;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;

    return CreateSRVFromWICFrame(frame.Get(), device);
}

// ============================================================
//  6. D3D11 初始化
// ============================================================
static bool CreateDeviceAndSwapChain()
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = g_r.width;
    sd.BufferDesc.Height = g_r.height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_r.hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL gotLevel = D3D_FEATURE_LEVEL_10_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &sd, &g_r.swapChain, &g_r.device, &gotLevel, &g_r.ctx);

    if (FAILED(hr))
    {
        g_r.swapChain.Reset(); g_r.device.Reset(); g_r.ctx.Reset();
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createFlags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &sd, &g_r.swapChain, &g_r.device, &gotLevel, &g_r.ctx);
        if (FAILED(hr)) return false;
    }
    return true;
}

static void CreateBackBuffer()
{
    g_r.backRTV.Reset();
    g_r.backDSV.Reset();
    g_r.backDepthTex.Reset();

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(g_r.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return;
    g_r.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_r.backRTV);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = g_r.width;
    desc.Height = g_r.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_r.device->CreateTexture2D(&desc, nullptr, &g_r.backDepthTex))) return;
    g_r.device->CreateDepthStencilView(g_r.backDepthTex.Get(), nullptr, &g_r.backDSV);
}

static void ResizeOffscreen(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (w == g_r.offW && h == g_r.offH) return;

    g_r.offRTV.Reset(); g_r.offSRV.Reset(); g_r.offColorTex.Reset();
    g_r.offDSV.Reset(); g_r.offDepthTex.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(g_r.device->CreateTexture2D(&td, nullptr, &g_r.offColorTex))) return;
    g_r.device->CreateRenderTargetView(g_r.offColorTex.Get(), nullptr, &g_r.offRTV);
    g_r.device->CreateShaderResourceView(g_r.offColorTex.Get(), nullptr, &g_r.offSRV);

    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_r.device->CreateTexture2D(&td, nullptr, &g_r.offDepthTex))) return;
    g_r.device->CreateDepthStencilView(g_r.offDepthTex.Get(), nullptr, &g_r.offDSV);

    g_r.offW = w;
    g_r.offH = h;
}

static bool CreatePipeline()
{
    ID3D11Device* dev = g_r.device.Get();

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompile(g_shaderSource, strlen(g_shaderSource),
        nullptr, nullptr, nullptr, "VSMain", "vs_5_0",
        flags, 0, &vsBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((const char*)errBlob->GetBufferPointer());
        return false;
    }
    hr = D3DCompile(g_shaderSource, strlen(g_shaderSource),
        nullptr, nullptr, nullptr, "PSMain", "ps_5_0",
        flags, 0, &psBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((const char*)errBlob->GetBufferPointer());
        return false;
    }

    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, &g_r.vs);
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, &g_r.ps);

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, pos),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(Vertex, uv),     D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    dev->CreateInputLayout(layout, ARRAYSIZE(layout),
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_r.inputLayout);

    D3D11_BUFFER_DESC cbd = {};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.ByteWidth = sizeof(PerFrameCB);
    dev->CreateBuffer(&cbd, nullptr, &g_r.cbPerFrame);
    cbd.ByteWidth = sizeof(PerObjectCB);
    dev->CreateBuffer(&cbd, nullptr, &g_r.cbPerObject);

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_BACK;
    rs.FrontCounterClockwise = FALSE;
    rs.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rs, &g_r.rsSolid);
    rs.FillMode = D3D11_FILL_WIREFRAME;
    dev->CreateRasterizerState(&rs, &g_r.rsWire);

    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    ds.StencilEnable = FALSE;
    dev->CreateDepthStencilState(&ds, &g_r.dsState);

    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&samp, &g_r.sampler);

    return true;
}

static void UploadMesh(const std::vector<Vertex>& verts,
    const std::vector<uint32_t>& indices)
{
    ID3D11Device* dev = g_r.device.Get();
    g_r.vb.Reset();
    g_r.ib.Reset();

    D3D11_BUFFER_DESC vbd = {};
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.ByteWidth = (UINT)(verts.size() * sizeof(Vertex));
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = {};
    vsd.pSysMem = verts.data();
    dev->CreateBuffer(&vbd, &vsd, &g_r.vb);

    D3D11_BUFFER_DESC ibd = {};
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.ByteWidth = (UINT)(indices.size() * sizeof(uint32_t));
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = {};
    isd.pSysMem = indices.data();
    dev->CreateBuffer(&ibd, &isd, &g_r.ib);

    g_r.indexCount = (UINT)indices.size();

    // 保存 CPU 副本供导出使用
    g_r.cpuVertices = verts;
    g_r.cpuIndices = indices;
}

// ============================================================
//  7. 默认球体
// ============================================================
static void LoadDefaultSphere()
{
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    const float R = 1.0f;
    const int segs = 48, rings = 32;

    for (int y = 0; y <= rings; ++y)
    {
        float v = (float)y / rings;
        float phi = v * XM_PI;
        for (int x = 0; x <= segs; ++x)
        {
            float u = (float)x / segs;
            float theta = u * XM_2PI;
            float px = R * sinf(phi) * cosf(theta);
            float py = R * cosf(phi);
            float pz = R * sinf(phi) * sinf(theta);
            verts.push_back({ {px,py,pz}, {px / R,py / R,pz / R}, {u,v} });
        }
    }
    for (int y = 0; y < rings; ++y)
        for (int x = 0; x < segs; ++x)
        {
            uint32_t a = (uint32_t)(y * (segs + 1) + x);
            uint32_t b = a + 1;
            uint32_t c = (uint32_t)((y + 1) * (segs + 1) + x);
            uint32_t d = c + 1;
            idx.insert(idx.end(), { a,c,b, b,c,d });
        }

    UploadMesh(verts, idx);

    g_r.drawCalls.clear();
    g_r.materials.clear();
    g_r.textures.clear();

    MaterialGPU m;
    m.baseColor = XMFLOAT4(0.85f, 0.55f, 0.25f, 1.0f);
    m.name = "default";
    m.reason = MaterialFallbackReason::NoTexture;
    g_r.materials.push_back(m);

    DrawCall dc;
    dc.indexStart = 0;
    dc.indexCount = (UINT)idx.size();
    dc.materialIdx = 0;
    g_r.drawCalls.push_back(dc);

    g_r.statusText = u8"已加载：默认球体";
    g_r.statusIsError = false;
}

// ============================================================
//  8. 模型 加载
// ============================================================
static const XMFLOAT4 kFallbackPalette[] = {
    { 0.80f, 0.60f, 0.40f, 1.0f },  // 琥珀
    { 0.40f, 0.65f, 0.85f, 1.0f },  // 天蓝
    { 0.55f, 0.80f, 0.50f, 1.0f },  // 草绿
    { 0.85f, 0.45f, 0.55f, 1.0f },  // 珊瑚
    { 0.70f, 0.55f, 0.85f, 1.0f },  // 紫罗兰
    { 0.85f, 0.80f, 0.45f, 1.0f },  // 芥末
    { 0.45f, 0.80f, 0.80f, 1.0f },  // 青蓝
    { 0.75f, 0.60f, 0.45f, 1.0f },  // 陶土
};

static XMFLOAT4 PickFallbackColor(const std::string& name, const Renderer& r)
{
    if (r.useHashBasedFallback && !name.empty())
    {
        uint32_t h = HashString(name.data(), name.size());
        return kFallbackPalette[h % ARRAYSIZE(kFallbackPalette)];
    }
    return r.defaultFallbackColor;
}
// 从材质中挑选"基础色"纹理，覆盖各种 FBX 导出风格
static ufbx_texture* PickBaseColorTexture(ufbx_material* mat)
{
    if (!mat) return nullptr;

    // 1) PBR base color（ufbx 优先归一化路径）
    if (mat->pbr.base_color.texture) return mat->pbr.base_color.texture;

    // 2) 旧版 FBX Diffuse
    if (mat->fbx.diffuse_color.texture) return mat->fbx.diffuse_color.texture;

    // 3) 遍历所有绑定的贴图，按属性名匹配
    if (mat->textures.count > 0)
    {
        for (size_t i = 0; i < mat->textures.count; ++i)
        {
            const ufbx_material_texture& mt = mat->textures.data[i];
            if (!mt.texture) continue;

            std::string name = UfbxStr(mt.material_prop);
            if (name == "DiffuseColor" || name == "Diffuse" ||
                name == "BaseColor" || name == "Base Color" ||
                name == "base_color" || name == "baseColor" ||
                name == "Maya|baseColor" ||
                name == "3dsMax|Parameters|base_color" ||
                name == "3dsMax|Parameters|base_color_map")
            {
                return mt.texture;
            }
        }

        // 4) 没匹配上，就用第一个非空纹理
        for (size_t i = 0; i < mat->textures.count; ++i)
        {
            if (mat->textures.data[i].texture)
                return mat->textures.data[i].texture;
        }
    }

    return nullptr;
}
static bool LoadFBX(const std::wstring& filePath)
{
    std::string utf8Path = WideToUtf8(filePath);

    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_left_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.generate_missing_normals = true;
    opts.load_external_files = true;
    // 处理手性转换
    opts.handedness_conversion_axis = UFBX_MIRROR_AXIS_Z;

    opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE_NO_FALLBACK;
    opts.geometry_transform_handling =
        UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY_NO_FALLBACK;

    ufbx_error err;
    ufbx_scene* scene = ufbx_load_file(utf8Path.c_str(), &opts, &err);
    if (!scene)
    {
        std::string msg = u8"FBX 加载失败: ";
        msg += UfbxStr(err.description);
        OutputDebugStringA(msg.c_str());
        g_r.statusText = msg;
        g_r.statusIsError = true;
        return false;
    }

    // ---------- 预存旧状态，失败时回滚 ----------
    auto oldDrawCalls = std::move(g_r.drawCalls);
    auto oldMaterials = std::move(g_r.materials);
    auto oldTextures = std::move(g_r.textures);
    g_r.drawCalls.clear();
    g_r.materials.clear();
    g_r.textures.clear();

    // ---------- 加载纹理 ----------
    std::unordered_map<ufbx_texture*, int> texMap;
    size_t texFailed = 0;

    for (size_t i = 0; i < scene->textures.count; ++i)
    {
        ufbx_texture* tex = scene->textures.data[i];
        ComPtr<ID3D11ShaderResourceView> srv;

        // 1) 优先使用内嵌数据
        if (tex->content.data && tex->content.size > 0)
        {
            srv = LoadTextureFromMemory(tex->content.data, tex->content.size,
                g_r.device.Get());
        }

        // 2) 回退到相对路径
        if (!srv && tex->relative_filename.length > 0)
        {
            std::wstring baseDir = GetDirectory(filePath);
            std::wstring relName = Utf8ToWide(UfbxStr(tex->relative_filename));
            srv = LoadTextureFromFile(baseDir + relName, g_r.device.Get());
        }

        // 3) 尝试绝对路径（有些 FBX 会把完整路径放进 absolute_filename）
        if (!srv && tex->absolute_filename.length > 0)
        {
            std::wstring absName = Utf8ToWide(UfbxStr(tex->absolute_filename));
            srv = LoadTextureFromFile(absName, g_r.device.Get());
        }

        if (srv)
        {
            texMap[tex] = (int)g_r.textures.size();
            g_r.textures.push_back(srv);
        }
        else
        {
            texFailed++;
            DebugLogUfbxString("texture load failed: ", tex->filename);
        }
    }

    // ---------- 加载材质（含回退色逻辑） ----------
    std::unordered_map<ufbx_material*, int> matMap;

    for (size_t i = 0; i < scene->materials.count; ++i)
    {
        ufbx_material* mat = scene->materials.data[i];
        MaterialGPU gpu;

        gpu.name = UfbxStr(mat->name);

        // base_color 数值
        bool hasBaseColorValue = (mat->pbr.base_color.has_value != 0);
        if (hasBaseColorValue)
        {
            ufbx_vec4 c = mat->pbr.base_color.value_vec4;
            gpu.baseColor = XMFLOAT4((float)c.x, (float)c.y, (float)c.z, (float)c.w);
        }
        else
        {
            gpu.baseColor = XMFLOAT4(1, 1, 1, 1);
        }

        // 尝试绑定纹理
        ufbx_texture* srcTex = PickBaseColorTexture(mat);
        bool loadedOk = false;

        if (srcTex && !g_r.forceSolidColor)
        {
            auto it = texMap.find(srcTex);
            if (it != texMap.end())
            {
                gpu.textureIndex = it->second;
                gpu.reason = MaterialFallbackReason::None;
                loadedOk = true;
            }
            else
            {
                gpu.textureIndex = -1;
                gpu.reason = MaterialFallbackReason::LoadFailed;
                gpu.missingTexturePath = UfbxStr(srcTex->filename);
            }
        }
        else if (!srcTex)
        {
            gpu.reason = MaterialFallbackReason::NoTexture;
        }
        else
        {
            // forceSolidColor 打开
            gpu.reason = MaterialFallbackReason::NoTexture;
        }

        // 未成功加载纹理 → 决定回退颜色
        if (!loadedOk)
        {
            if (!hasBaseColorValue)
            {
                if (g_r.forceSolidColor)
                {
                    gpu.baseColor = PickFallbackColor(gpu.name, g_r);
                }
                else
                {
                    gpu.baseColor = PickFallbackColor(gpu.name, g_r);
                }
                gpu.reason = (gpu.reason == MaterialFallbackReason::None)
                    ? MaterialFallbackReason::NoBaseColor
                    : gpu.reason;
            }
        }

        matMap[mat] = (int)g_r.materials.size();
        g_r.materials.push_back(gpu);
    }

    if (g_r.materials.empty())
    {
        MaterialGPU m;
        m.baseColor = g_r.defaultFallbackColor;
        m.name = "(no material)";
        m.reason = MaterialFallbackReason::NoBaseColor;
        g_r.materials.push_back(m);
    }

    // ---------- 组装顶点 / 索引 / DrawCall ----------
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;

    size_t nodeWithMeshCount = 0;

    for (size_t ni = 0; ni < scene->nodes.count; ++ni)
    {
        ufbx_node* node = scene->nodes.data[ni];
        if (!node->mesh) continue;
        ufbx_mesh* mesh = node->mesh;
        if (mesh->num_indices == 0) continue;

        nodeWithMeshCount++;

        const uint32_t vertexBase = (uint32_t)vertices.size();

        // ---------- 顶点：按 index 展开 ----------
        // 位置/法线/UV 全部通过 indices.data[i] 索引，避免越界/错位。
        ufbx_matrix nrmMat = ufbx_matrix_for_normals(&node->geometry_to_world);

        for (size_t i = 0; i < mesh->num_indices; ++i)
        {
            // 位置（ufbx 保证 vertex_position.indices 有效）
            uint32_t p_ix = mesh->vertex_position.indices.data[i];
            ufbx_vec3 p = mesh->vertex_position.values.data[p_ix];
            ufbx_vec3 wp = ufbx_transform_position(&node->geometry_to_world, p);

            // 法线
            ufbx_vec3 n = { 0.0, 1.0, 0.0 };
            ufbx_vec3 wn = n;
            if (mesh->vertex_normal.exists && mesh->vertex_normal.indices.data)
            {
                uint32_t n_ix = mesh->vertex_normal.indices.data[i];
                if (n_ix < mesh->vertex_normal.values.count)
                {
                    n = mesh->vertex_normal.values.data[n_ix];
                    wn = ufbx_transform_direction(&nrmMat, n);
                }
            }

            // UV（V 轴翻转，FBX 左下原点 -> D3D 左上原点）
            ufbx_vec2 uv = { 0.0, 0.0 };
            if (mesh->vertex_uv.exists && mesh->vertex_uv.indices.data)
            {
                uint32_t uv_ix = mesh->vertex_uv.indices.data[i];
                if (uv_ix < mesh->vertex_uv.values.count)
                    uv = mesh->vertex_uv.values.data[uv_ix];
            }

            Vertex v;
            v.pos = XMFLOAT3((float)wp.x, (float)wp.y, (float)wp.z);
            v.normal = XMFLOAT3((float)wn.x, (float)wn.y, (float)wn.z);
            v.uv = XMFLOAT2((float)uv.x, 1.0f - (float)uv.y);
            vertices.push_back(v);
        }

        // ---------- 按材质分桶收集索引 ----------
        // 关键：一个材质可能对应多个不连续的面段，必须先分桶再拼接。
        struct MatBucket
        {
            int globalMat = 0;
            std::vector<uint32_t> idx;
        };
        std::vector<MatBucket> buckets;
        std::unordered_map<int, size_t> matToBucket;

        for (size_t fi = 0; fi < mesh->num_faces; ++fi)
        {
            ufbx_face face = mesh->faces.data[fi];
            if (face.num_indices < 3) continue;

            uint32_t localMatIdx = 0;
            if (mesh->face_material.data && fi < mesh->face_material.count)
                localMatIdx = mesh->face_material.data[fi];

            ufbx_material* mat = nullptr;
            if (mesh->materials.data && localMatIdx < mesh->materials.count)
                mat = mesh->materials.data[localMatIdx];

            int globalMat = 0;
            auto it = matMap.find(mat);
            if (it != matMap.end()) globalMat = it->second;

            size_t bucketIx;
            auto it2 = matToBucket.find(globalMat);
            if (it2 == matToBucket.end())
            {
                MatBucket b;
                b.globalMat = globalMat;
                buckets.push_back(std::move(b));
                bucketIx = buckets.size() - 1;
                matToBucket[globalMat] = bucketIx;
            }
            else
            {
                bucketIx = it2->second;
            }

            auto& bucket = buckets[bucketIx].idx;

            // 扇形三角化。因为我们是按 index 展开顶点，
            // mesh 索引 i 对应的顶点号就是 vertexBase + i。
            const uint32_t begin = face.index_begin;
            const uint32_t count = face.num_indices;
            for (uint32_t k = 1; k + 1 < count; ++k)
            {
                bucket.push_back(vertexBase + (begin + 0));
                bucket.push_back(vertexBase + (begin + k));
                bucket.push_back(vertexBase + (begin + k + 1));
            }
        }

        // 拼接并生成 DrawCall
        for (auto& b : buckets)
        {
            if (b.idx.empty()) continue;
            DrawCall dc;
            dc.indexStart = (UINT)indices.size();
            dc.indexCount = (UINT)b.idx.size();
            dc.materialIdx = b.globalMat;
            indices.insert(indices.end(), b.idx.begin(), b.idx.end());
            g_r.drawCalls.push_back(dc);
        }
    }

    ufbx_free_scene(scene);

    if (vertices.empty() || indices.empty() || g_r.drawCalls.empty())
    {
        // 回滚到旧状态
        g_r.drawCalls = std::move(oldDrawCalls);
        g_r.materials = std::move(oldMaterials);
        g_r.textures = std::move(oldTextures);
        g_r.statusText = "FBX 中未找到任何网格";
        g_r.statusIsError = true;
        return false;
    }

    UploadMesh(vertices, indices);

    // ---------- 自动适配相机 ----------
    float maxExtent = 0.0f;
    for (const auto& v : vertices)
    {
        maxExtent = std::max(maxExtent,
            std::max(std::fabs(v.pos.x),
                std::max(std::fabs(v.pos.y), std::fabs(v.pos.z))));
    }
    g_r.camera.target = XMFLOAT3(0, 0, 0);
    g_r.camera.distance = std::max(1.0f, maxExtent * 3.0f);
    g_r.camera.Clamp();

    // ---------- 状态更新 ----------
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            u8"已加载: %zu 节点 / %zu 材质 / %zu 纹理(失败 %zu) \n渲染统计： %zu DrawCall / %zu 顶点",
            nodeWithMeshCount,
            g_r.materials.size(),
            g_r.textures.size(),
            texFailed,
            g_r.drawCalls.size(),
            vertices.size());
        g_r.statusText = buf;
        g_r.statusIsError = false;
    }

    return true;
}

static bool LoadGMD(const std::wstring& filePath)
{
    // -------- 从宽路径读入内存（避免 GmdIO.h 里窄路径的编码问题） --------
    std::ifstream f(filePath.c_str(), std::ios::binary | std::ios::ate);
    if (!f)
    {
        g_r.statusText = "GMD 加载失败: 无法打开文件";
        g_r.statusIsError = true;
        return false;
    }
    auto sz = f.tellg();
    if (sz <= 0)
    {
        g_r.statusText = "GMD 加载失败: 文件为空";
        g_r.statusIsError = true;
        return false;
    }

    std::vector<uint8_t> fileBytes(static_cast<size_t>(sz));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(fileBytes.data()), sz))
    {
        g_r.statusText = "GMD 加载失败: 读取失败";
        g_r.statusIsError = true;
        return false;
    }
    f.close();

    // -------- 解析 --------
    gmd::GmdReader reader;
    std::string err;
    if (!reader.OpenFromMemory(fileBytes.data(), fileBytes.size(), &err))
    {
        std::string msg = "GMD 解析失败: ";
        msg += err;
        OutputDebugStringA(msg.c_str());
        g_r.statusText = msg;
        g_r.statusIsError = true;
        return false;
    }

    const gmd::GmdModel& model = reader.Model();

    // -------- 预存旧状态，失败时回滚 --------
    auto oldDrawCalls = std::move(g_r.drawCalls);
    auto oldMaterials = std::move(g_r.materials);
    auto oldTextures = std::move(g_r.textures);
    g_r.drawCalls.clear();
    g_r.materials.clear();
    g_r.textures.clear();

    // ============================================================
    //  纹理
    // ============================================================
    std::unordered_map<uint32_t, int> texMap;
    size_t texFailed = 0;

    for (size_t i = 0; i < model.textures.size(); ++i)
    {
        const auto& T = model.textures[i];
        ComPtr<ID3D11ShaderResourceView> srv;

        // 1) 内嵌数据（TEXD 块）
        if (!T.embeddedData.empty())
        {
            srv = LoadTextureFromMemory(T.embeddedData.data(),
                T.embeddedData.size(),
                g_r.device.Get());
        }

        // 2) 外部 URI
        if (!srv && !T.uri.empty())
        {
            std::wstring baseDir = GetDirectory(filePath);
            std::wstring uriW = Utf8ToWide(T.uri);

            // 优先尝试相对于 .gmd 文件
            srv = LoadTextureFromFile(baseDir + uriW, g_r.device.Get());

            // 再尝试绝对路径
            if (!srv)
                srv = LoadTextureFromFile(uriW, g_r.device.Get());
        }

        if (srv)
        {
            texMap[static_cast<uint32_t>(i)] = static_cast<int>(g_r.textures.size());
            g_r.textures.push_back(srv);
        }
        else
        {
            texFailed++;
            std::string log = "gmd texture load failed: " + T.name
                + " (uri=" + T.uri + ")\n";
            OutputDebugStringA(log.c_str());
        }
    }

    // ============================================================
    //  材质
    // ============================================================
    std::unordered_map<uint32_t, int> matMap;

    for (size_t i = 0; i < model.materials.size(); ++i)
    {
        const auto& M = model.materials[i];
        MaterialGPU gpu;
        gpu.name = M.name;
        gpu.baseColor = XMFLOAT4(M.mat.baseColorFactor[0],
            M.mat.baseColorFactor[1],
            M.mat.baseColorFactor[2],
            M.mat.baseColorFactor[3]);

        bool loadedOk = false;
        uint32_t texIdx = M.mat.baseColorTexture.textureIndex;

        if (texIdx != gmd::kInvalidIndex && !g_r.forceSolidColor)
        {
            auto it = texMap.find(texIdx);
            if (it != texMap.end())
            {
                gpu.textureIndex = it->second;
                gpu.reason = MaterialFallbackReason::None;
                loadedOk = true;
            }
            else
            {
                gpu.reason = MaterialFallbackReason::LoadFailed;
                if (texIdx < model.textures.size())
                {
                    const auto& T = model.textures[texIdx];
                    gpu.missingTexturePath = T.uri.empty() ? T.name : T.uri;
                }
            }
        }
        else
        {
            gpu.reason = MaterialFallbackReason::NoTexture;
        }

        matMap[static_cast<uint32_t>(i)] = static_cast<int>(g_r.materials.size());
        g_r.materials.push_back(gpu);
    }

    if (g_r.materials.empty())
    {
        MaterialGPU m;
        m.baseColor = g_r.defaultFallbackColor;
        m.name = "(no material)";
        m.reason = MaterialFallbackReason::NoBaseColor;
        g_r.materials.push_back(m);
    }

    // ============================================================
    //  顶点 / 索引
    // ============================================================
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> srcIndices; // 从文件读取的原始索引（未重基准）
    std::vector<uint32_t> indices; // 最终写入 D3D11 的索引缓冲

    // 找出总顶点数（多个流取最大）
    uint32_t totalVertexCount = 0;
    for (const auto& vs : model.vertexStreams)
        totalVertexCount = std::max(totalVertexCount, vs.vertexCount);

    if (totalVertexCount == 0 || model.vertexStreams.empty())
    {
        g_r.drawCalls = std::move(oldDrawCalls);
        g_r.materials = std::move(oldMaterials);
        g_r.textures = std::move(oldTextures);
        g_r.statusText = "GMD 中没有任何顶点数据";
        g_r.statusIsError = true;
        return false;
    }

    // 默认值
    vertices.resize(totalVertexCount);
    for (auto& v : vertices)
    {
        v.pos = XMFLOAT3(0, 0, 0);
        v.normal = XMFLOAT3(0, 1, 0);
        v.uv = XMFLOAT2(0, 0);
    }

    // ---- 从每个顶点流提取属性 ----
    for (const auto& vs : model.vertexStreams)
    {
        if (vs.dataOffset + vs.dataSize > model.vertexData.size())
            continue;

        if (vs.quantization != 0)
        {
            // 不支持量化格式，跳过该流
            continue;
        }

        const uint8_t* base = model.vertexData.data() + vs.dataOffset;
        const uint32_t mask = vs.attributeMask;
        const uint32_t stride = vs.stride;

        uint32_t off = 0;
        int posOff = -1, nrmOff = -1, uvOff = -1;

        if (mask & gmd::ATTR_POSITION) { posOff = static_cast<int>(off); off += 12; }
        if (mask & gmd::ATTR_NORMAL) { nrmOff = static_cast<int>(off); off += 12; }
        if (mask & gmd::ATTR_TANGENT) { off += 16; }
        if (mask & gmd::ATTR_TEXCOORD0) { uvOff = static_cast<int>(off); off += 8; }
        if (mask & gmd::ATTR_TEXCOORD1) { off += 8; }
        if (mask & gmd::ATTR_COLOR) { off += 16; }
        if (mask & gmd::ATTR_JOINTS) { off += 8; }
        if (mask & gmd::ATTR_WEIGHTS) { off += 16; }

        if (off > stride)
            continue;   // 布局不匹配，跳过

        uint32_t n = std::min(vs.vertexCount, totalVertexCount);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint8_t* p = base + static_cast<size_t>(i) * stride;
            Vertex& v = vertices[i];

            if (posOff >= 0 && static_cast<uint32_t>(posOff) + 12 <= stride)
                std::memcpy(&v.pos, p + posOff, 12);

            if (nrmOff >= 0 && static_cast<uint32_t>(nrmOff) + 12 <= stride)
                std::memcpy(&v.normal, p + nrmOff, 12);

            if (uvOff >= 0 && static_cast<uint32_t>(uvOff) + 8 <= stride)
            {
                float u = 0.0f, w = 0.0f;
                std::memcpy(&u, p + uvOff, 4);
                std::memcpy(&w, p + uvOff + 4, 4);
                // V 轴翻转：与 FBX 路径保持一致（如果你的格式按 D3D 约定存储可去掉 1.0f -）
                v.uv = XMFLOAT2(u, 1.0f - w);
            }
        }
    }

    // ---- 读取原始索引 ----
    if (model.indexBuffers.empty())
    {
        // 无索引：顺序三角形，按顶点数补齐
        srcIndices.resize(totalVertexCount);
        for (uint32_t i = 0; i < totalVertexCount; ++i)
            srcIndices[i] = i;
    }
    else
    {
        const auto& ib = model.indexBuffers[0];
        if (ib.dataOffset + ib.dataSize > model.indexData.size())
        {
            g_r.drawCalls = std::move(oldDrawCalls);
            g_r.materials = std::move(oldMaterials);
            g_r.textures = std::move(oldTextures);
            g_r.statusText = "GMD 索引数据越界";
            g_r.statusIsError = true;
            return false;
        }

        const uint8_t* idxData = model.indexData.data() + ib.dataOffset;
        uint32_t count = ib.indexCount;
        srcIndices.resize(count);

        if (ib.indexType == gmd::INDEX_TYPE_UINT16)
        {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(idxData);
            for (uint32_t i = 0; i < count; ++i)
                srcIndices[i] = src[i];
        }
        else
        {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(idxData);
            for (uint32_t i = 0; i < count; ++i)
                srcIndices[i] = src[i];
        }
    }

    // ============================================================
    //  按子网格切分索引，展开为最终 Vertex/Index 缓冲
    // ============================================================
    bool skipLodFilter = model.lods.empty();

    if (model.submeshes.empty())
    {
        // 没有子网格定义：整体画一次，用材质 0
        DrawCall dc;
        dc.indexStart = 0;
        dc.indexCount = static_cast<UINT>(srcIndices.size());
        dc.materialIdx = 0;

        indices = srcIndices;
        if (dc.indexCount > 0)
            g_r.drawCalls.push_back(dc);
    }
    else
    {
        for (const auto& sm : model.submeshes)
        {
            // 有 LOD 信息时仅绘制 LOD0
            if (!skipLodFilter && sm.lodIndex != 0) continue;

            if (static_cast<uint64_t>(sm.indexOffset) + sm.indexCount > srcIndices.size())
                continue;

            DrawCall dc;
            dc.indexStart = static_cast<UINT>(indices.size());
            dc.indexCount = sm.indexCount;
            dc.materialIdx = 0;

            auto it = matMap.find(sm.materialIndex);
            if (it != matMap.end()) dc.materialIdx = it->second;

            // 依据 submesh.vertexOffset 重基准（等价于 D3D DrawIndexed 的 BaseVertex）
            for (uint32_t k = 0; k < sm.indexCount; ++k)
            {
                uint32_t v = srcIndices[sm.indexOffset + k] + sm.vertexOffset;
                if (v >= totalVertexCount) v = 0;   // 越界保护
                indices.push_back(v);
            }

            if (dc.indexCount > 0)
                g_r.drawCalls.push_back(dc);
        }
    }

    // 校验
    if (indices.empty() && !srcIndices.empty())
    {
        // 子网格异常时退化为整体绘制
        indices = srcIndices;
        DrawCall dc;
        dc.indexStart = 0;
        dc.indexCount = static_cast<UINT>(indices.size());
        dc.materialIdx = 0;
        g_r.drawCalls.clear();
        g_r.drawCalls.push_back(dc);
    }

    if (vertices.empty() || indices.empty() || g_r.drawCalls.empty())
    {
        g_r.drawCalls = std::move(oldDrawCalls);
        g_r.materials = std::move(oldMaterials);
        g_r.textures = std::move(oldTextures);
        g_r.statusText = "GMD 中未找到可渲染的网格";
        g_r.statusIsError = true;
        return false;
    }

    UploadMesh(vertices, indices);

    // 相机自动适配
    float maxExtent = 0.0f;
    for (const auto& v : vertices)
    {
        maxExtent = std::max(maxExtent,
            std::max(std::fabs(v.pos.x),
                std::max(std::fabs(v.pos.y), std::fabs(v.pos.z))));
    }
    g_r.camera.target = XMFLOAT3(0, 0, 0);
    g_r.camera.distance = std::max(1.0f, maxExtent * 3.0f);
    g_r.camera.Clamp();

    // 状态
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "已加载 GMD: %zu 材质 / %zu 纹理(失败 %zu) / %zu DrawCall / %zu 顶点",
            g_r.materials.size(),
            g_r.textures.size(),
            texFailed,
            g_r.drawCalls.size(),
            vertices.size());
        g_r.statusText = buf;
        g_r.statusIsError = false;
    }

    return true;
}

// 按扩展名分发到具体加载器
static bool LoadModel(const std::wstring& filePath)
{
    std::wstring ext;
    size_t dot = filePath.find_last_of(L'.');
    if (dot != std::wstring::npos)
        ext = filePath.substr(dot);

    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    if (ext == L".gmd")
        return LoadGMD(filePath);

    return LoadFBX(filePath);
}


// ============================================================
//  9. 3D 渲染
// ============================================================
static void RenderScene3D()
{
    if (!g_r.offRTV || !g_r.offDSV || g_r.offW <= 0 || g_r.offH <= 0) return;
    if (g_r.drawCalls.empty()) return;

    ID3D11DeviceContext* ctx = g_r.ctx.Get();

    ID3D11RenderTargetView* rtvs[1] = { g_r.offRTV.Get() };
    ctx->OMSetRenderTargets(1, rtvs, g_r.offDSV.Get());

    const float clearColor[4] = { 0.10f, 0.11f, 0.14f, 1.0f };
    ctx->ClearRenderTargetView(g_r.offRTV.Get(), clearColor);
    ctx->ClearDepthStencilView(g_r.offDSV.Get(),
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)g_r.offW;
    vp.Height = (float)g_r.offH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ctx->RSSetState(g_r.wireframe ? g_r.rsWire.Get() : g_r.rsSolid.Get());
    ctx->OMSetDepthStencilState(g_r.dsState.Get(), 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(g_r.inputLayout.Get());
    ctx->VSSetShader(g_r.vs.Get(), nullptr, 0);
    ctx->PSSetShader(g_r.ps.Get(), nullptr, 0);
    ctx->PSSetSamplers(0, 1, g_r.sampler.GetAddressOf());

    // PerFrame
    PerFrameCB pf;
    XMMATRIX view = g_r.camera.GetView();
    XMMATRIX proj = g_r.camera.GetProj((float)g_r.offW / (float)g_r.offH);
    XMMATRIX vpm = XMMatrixMultiply(view, proj);
    XMStoreFloat4x4(&pf.viewProj, XMMatrixTranspose(vpm));

    XMFLOAT3 camPos = g_r.camera.GetPosition();
    pf.cameraPos = XMFLOAT4(camPos.x, camPos.y, camPos.z, 1.0f);

    float lcp = cosf(g_r.lightPitch);
    pf.lightDir = XMFLOAT4(
        -lcp * sinf(g_r.lightYaw),
        -sinf(g_r.lightPitch),
        -lcp * cosf(g_r.lightYaw), 0.0f);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(g_r.cbPerFrame.Get(), 0,
        D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &pf, sizeof(pf));
        ctx->Unmap(g_r.cbPerFrame.Get(), 0);
    }
    ctx->VSSetConstantBuffers(0, 1, g_r.cbPerFrame.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, g_r.cbPerFrame.GetAddressOf());

    // IA
    UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer* vbs[1] = { g_r.vb.Get() };
    ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    ctx->IASetIndexBuffer(g_r.ib.Get(), DXGI_FORMAT_R32_UINT, 0);

    // 世界矩阵
    XMMATRIX world = XMMatrixRotationY(g_r.modelRotationY);
    XMFLOAT4X4 worldT;
    XMStoreFloat4x4(&worldT, XMMatrixTranspose(world));

    ID3D11ShaderResourceView* nullSRV = nullptr;

    for (const DrawCall& dc : g_r.drawCalls)
    {
        if (dc.materialIdx < 0 || dc.materialIdx >= (int)g_r.materials.size())
            continue;
        const MaterialGPU& mat = g_r.materials[dc.materialIdx];

        PerObjectCB po;
        po.world = worldT;

        if (g_r.wireframe)
        {
            // 线框模式：强制纯色线条，关闭纹理采样，忽略光照/贴图影响
            po.color = g_r.wireframeColor;
            po.useTexture = 0.0f;
            po.wireframe = 1.0f;
        }
        else
        {
            po.color = mat.baseColor;
            po.useTexture = (mat.textureIndex >= 0) ? 1.0f : 0.0f;
            po.wireframe = 0.0f;
        }
        po.pad2 = XMFLOAT2(0, 0);

        if (SUCCEEDED(ctx->Map(g_r.cbPerObject.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            memcpy(mapped.pData, &po, sizeof(po));
            ctx->Unmap(g_r.cbPerObject.Get(), 0);
        }
        ctx->VSSetConstantBuffers(1, 1, g_r.cbPerObject.GetAddressOf());
        ctx->PSSetConstantBuffers(1, 1, g_r.cbPerObject.GetAddressOf());

        if (mat.textureIndex >= 0 && mat.textureIndex < (int)g_r.textures.size())
        {
            ID3D11ShaderResourceView* srv = g_r.textures[mat.textureIndex].Get();
            ctx->PSSetShaderResources(0, 1, &srv);
        }
        else
        {
            ctx->PSSetShaderResources(0, 1, &nullSRV);
        }

        ctx->DrawIndexed(dc.indexCount, dc.indexStart, 0);
    }

    ctx->PSSetShaderResources(0, 1, &nullSRV);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// ============================================================
// 10. 文件对话框
// ============================================================
static void OpenFileDialog()
{
    static bool comInit = false;
    if (!comInit)
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        comInit = true;
    }

    ComPtr<IFileOpenDialog> dlg;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&dlg));
    if (FAILED(hr)) return;

    COMDLG_FILTERSPEC types[] = {
        { L"支持的模型 (*.fbx;*.gmd)", L"*.fbx;*.gmd" },
        { L"所有文件", L"*.*"   },
    };
    dlg->SetFileTypes(ARRAYSIZE(types), types);

    hr = dlg->Show(g_r.hwnd);
    if (FAILED(hr)) return;

    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return;

    PWSTR pszPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) return;

    g_r.pendingFile = pszPath;
    g_r.hasPendingFile = true;
    CoTaskMemFree(pszPath);
}

static bool ShowSaveGmdDialog(std::wstring& outPath)
{
    ComPtr<IFileSaveDialog> dlg;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&dlg));
    if (FAILED(hr)) return false;

    COMDLG_FILTERSPEC types[] = {
        { L"GMD 模型 (*.gmd)", L"*.gmd" },
    };
    dlg->SetFileTypes(ARRAYSIZE(types), types);
    dlg->SetDefaultExtension(L"gmd");
    dlg->SetFileName(L"export.gmd");

    hr = dlg->Show(g_r.hwnd);
    if (FAILED(hr)) return false;

    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return false;

    PWSTR pszPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) return false;

    outPath = pszPath;
    CoTaskMemFree(pszPath);
    return true;
}

static void ExportCurrentModelToGMD()
{
    if (g_r.cpuVertices.empty() || g_r.cpuIndices.empty() || g_r.drawCalls.empty())
    {
        MessageBoxW(g_r.hwnd,
            L"当前没有可导出的模型。请先加载 FBX 或 GMD 文件。",
            L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring wpath;
    if (!ShowSaveGmdDialog(wpath)) return;

    std::string utf8Path = WideToUtf8(wpath);

    // ---------- 收集材质 ----------
    std::vector<gmd::GmdExportInput::MaterialInput> matIn(g_r.materials.size());
    for (size_t i = 0; i < g_r.materials.size(); ++i)
    {
        const auto& m = g_r.materials[i];
        matIn[i].name = m.name.c_str();
        matIn[i].baseColor[0] = m.baseColor.x;
        matIn[i].baseColor[1] = m.baseColor.y;
        matIn[i].baseColor[2] = m.baseColor.z;
        matIn[i].baseColor[3] = m.baseColor.w;
        matIn[i].textureIndex = m.textureIndex;
    }

    // ---------- 收集子网格 ----------
    std::vector<gmd::GmdExportInput::SubmeshInput> subIn(g_r.drawCalls.size());
    for (size_t i = 0; i < g_r.drawCalls.size(); ++i)
    {
        subIn[i].indexOffset = g_r.drawCalls[i].indexStart;
        subIn[i].indexCount = g_r.drawCalls[i].indexCount;
        subIn[i].materialIndex = g_r.drawCalls[i].materialIdx;
    }

    // ---------- 收集纹理 SRV ----------
    std::vector<ID3D11ShaderResourceView*> srvs;
    std::vector<std::string>               texNameStore;
    std::vector<const char*>               texNames;
    srvs.reserve(g_r.textures.size());
    texNameStore.reserve(g_r.textures.size());
    texNames.reserve(g_r.textures.size());

    for (size_t i = 0; i < g_r.textures.size(); ++i)
    {
        srvs.push_back(g_r.textures[i].Get());
        texNameStore.push_back("texture_" + std::to_string(i));
    }
    for (size_t i = 0; i < texNameStore.size(); ++i)
        texNames.push_back(texNameStore[i].c_str());

    // ---------- 构建输入 ----------
    gmd::GmdExportInput in = {};
    in.vertexData = g_r.cpuVertices.data();
    in.vertexCount = static_cast<uint32_t>(g_r.cpuVertices.size());
    in.vertexStride = sizeof(Vertex);

    in.indices = g_r.cpuIndices.data();
    in.indexCount = static_cast<uint32_t>(g_r.cpuIndices.size());

    in.materials = matIn.data();
    in.materialCount = static_cast<uint32_t>(matIn.size());

    in.submeshes = subIn.data();
    in.submeshCount = static_cast<uint32_t>(subIn.size());

    in.textureSRVs = srvs.empty() ? nullptr : srvs.data();
    in.textureNames = texNames.empty() ? nullptr : texNames.data();
    in.textureCount = static_cast<uint32_t>(srvs.size());

    in.device = g_r.device.Get();

    // ---------- 导出 ----------
    std::string err;
    if (gmd::ExportToFile(utf8Path, in, &err))
    {
        std::string msg = "导出成功: " + utf8Path;
        g_r.statusText = msg;
        g_r.statusIsError = false;
        OutputDebugStringA(msg.c_str());
        MessageBoxW(g_r.hwnd,
            L"导出成功。",
            L"提示", MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        std::string msg = "GMD 导出失败: " + err;
        g_r.statusText = msg;
        g_r.statusIsError = true;
        OutputDebugStringA(msg.c_str());

        std::wstring werr = Utf8ToWide(msg);
        MessageBoxW(g_r.hwnd, werr.c_str(),
            L"导出失败", MB_OK | MB_ICONERROR);
    }
}

// ============================================================
// 11. ImGui UI
// ============================================================
static void DrawMainMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu(u8"文件"))
    {
        if (ImGui::MenuItem(u8"打开 FBX...", "Ctrl+O"))
            OpenFileDialog();

        {
            bool canExport = !g_r.cpuVertices.empty() &&
                !g_r.cpuIndices.empty() &&
                !g_r.drawCalls.empty();
            if (!canExport)
                ImGui::BeginDisabled();
            if (ImGui::MenuItem(u8"导出为 .gmd...", "Ctrl+E"))
                ExportCurrentModelToGMD();
            if (!canExport)
                ImGui::EndDisabled();
        }

        ImGui::Separator();

        if (ImGui::MenuItem(u8"退出", "Alt+F4"))
            g_running = false;

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(u8"视图"))
    {
        ImGui::MenuItem(u8"控制面板", nullptr, &g_r.showPanel);
        ImGui::MenuItem(u8"线框模式", nullptr, &g_r.wireframe);
        ImGui::EndMenu();
    }

    ImGuiIO& io = ImGui::GetIO();
    char fps[64];
    if (io.Framerate > 0.01f)
        snprintf(fps, sizeof(fps), "%.1f FPS (%.2f ms)",
            io.Framerate, 1000.0f / io.Framerate);
    else
        snprintf(fps, sizeof(fps), "--- FPS");

    float w = ImGui::CalcTextSize(fps).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - 20);
    ImGui::TextUnformatted(fps);

    ImGui::EndMainMenuBar();
}

static void HandleViewportInput(bool hovered)
{
    if (!hovered) return;

    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        g_r.camera.yaw += io.MouseDelta.x * 0.008f;
        g_r.camera.pitch += io.MouseDelta.y * 0.008f;
        g_r.camera.Clamp();
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
        ImGui::IsMouseDragging(ImGuiMouseButton_Right))
    {
        XMVECTOR right = XMVectorSet(-cosf(g_r.camera.yaw), 0, sinf(g_r.camera.yaw), 0);
        XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        float    s = g_r.camera.distance * 0.0015f;

        XMVECTOR delta = XMVectorScale(right, -io.MouseDelta.x * s);
        delta = XMVectorAdd(delta, XMVectorScale(up, io.MouseDelta.y * s));

        XMFLOAT3 d; XMStoreFloat3(&d, delta);
        g_r.camera.target.x += d.x;
        g_r.camera.target.y += d.y;
        g_r.camera.target.z += d.z;
    }
    if (io.MouseWheel != 0.0f)
    {
        g_r.camera.distance *= powf(0.85f, io.MouseWheel);
        g_r.camera.Clamp();
    }
}

static void DrawViewportWindow()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("3DView", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int vpW = std::max(1, (int)avail.x);
    int vpH = std::max(1, (int)avail.y);

    ResizeOffscreen(vpW, vpH);
    RenderScene3D();

    if (g_r.offSRV)
    {
        ImGui::Image(
            (ImTextureID)(intptr_t)g_r.offSRV.Get(),
            ImVec2((float)vpW, (float)vpH),
            ImVec2(0, 0), ImVec2(1, 1),
            ImVec4(1, 1, 1, 1), ImVec4(1, 1, 1, 1));
    }

    bool hovered = ImGui::IsItemHovered();
    HandleViewportInput(hovered);

    ImGui::SetCursorPos(ImVec2(10, 10));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 0.8f),
        u8"左键拖动旋转 | 中/右键平移 | 滚轮缩放");

    ImGui::End();
    ImGui::PopStyleVar();
}

static void DrawSidePanel()
{
    ImGui::Begin(u8"控制面板", &g_r.showPanel,
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse);

    // 状态栏
    if (!g_r.statusText.empty())
    {
        ImVec4 col = g_r.statusIsError
            ? ImVec4(1.0f, 0.5f, 0.5f, 1.0f)
            : ImVec4(0.7f, 0.9f, 0.7f, 1.0f);
        ImGui::TextColored(col, "%s", g_r.statusText.c_str());
        ImGui::Separator();
    }

    if (ImGui::CollapsingHeader(u8"模型", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox(u8"自动旋转", &g_r.autoRotate);
        ImGui::SliderFloat(u8"旋转 Y", &g_r.modelRotationY,
            -XM_PI, XM_PI, "%.3f rad");
    }

    if (ImGui::CollapsingHeader(u8"相机", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat(u8"距离", &g_r.camera.distance, 0.1f, 200.0f, "%.2f");
        ImGui::SliderFloat("Yaw", &g_r.camera.yaw, -XM_PI, XM_PI, "%.2f rad");
        ImGui::SliderFloat("Pitch", &g_r.camera.pitch, -1.5f, 1.5f, "%.2f rad");
        ImGui::SliderFloat("FOV", &g_r.camera.fovY, 20.0f, 120.0f, "%.0f deg");
        if (ImGui::Button(u8"重置相机")) g_r.camera = Camera();
    }

    if (ImGui::CollapsingHeader(u8"光照", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat(u8"光 Yaw", &g_r.lightYaw, -XM_PI, XM_PI, "%.2f");
        ImGui::SliderFloat(u8"光 Pitch", &g_r.lightPitch, -1.5f, 1.5f, "%.2f");
    }

    if (ImGui::CollapsingHeader(u8"材质回退", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox(u8"强制纯色 (调试)", &g_r.forceSolidColor);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"打开后所有材质都忽略贴图，改用回退色。\n"
                u8"由于在加载时决定，需重新加载模型才生效。");
        ImGui::Checkbox(u8"按材质名 hash 派生回退色", &g_r.useHashBasedFallback);
        ImGui::ColorEdit3(u8"默认兜底色",
            &g_r.defaultFallbackColor.x,
            ImGuiColorEditFlags_NoAlpha);
    }

    if (ImGui::CollapsingHeader(u8"材质", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text(u8"材质数: %d", (int)g_r.materials.size());
        ImGui::Text(u8"纹理数: %d", (int)g_r.textures.size());
        ImGui::Text("DrawCall: %d", (int)g_r.drawCalls.size());

        for (size_t i = 0; i < g_r.materials.size(); ++i)
        {
            const MaterialGPU& m = g_r.materials[i];
            ImGui::PushID((int)i);

            // 颜色块 + 名字
            ImVec4 col(m.baseColor.x, m.baseColor.y, m.baseColor.z, 1.0f);
            ImGui::ColorButton("##c", col, ImGuiColorEditFlags_NoAlpha,
                ImVec2(16, 16));
            ImGui::SameLine();

            char label[256];
            snprintf(label, sizeof(label), "[%d] %s",
                (int)i, m.name.empty() ? "(unnamed)" : m.name.c_str());

            if (ImGui::TreeNode(label))
            {
                switch (m.reason)
                {
                case MaterialFallbackReason::None:
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                        u8"纹理: #%d", m.textureIndex);
                    if (m.textureIndex >= 0 &&
                        m.textureIndex < (int)g_r.textures.size())
                    {
                        ImGui::Image(
                            (ImTextureID)(intptr_t)g_r.textures[m.textureIndex].Get(),
                            ImVec2(96, 96));
                    }
                    break;
                case MaterialFallbackReason::NoTexture:
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                        u8"无基础色贴图，使用纯色");
                    break;
                case MaterialFallbackReason::LoadFailed:
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                        u8"纹理加载失败，使用纯色");
                    if (!m.missingTexturePath.empty())
                        ImGui::TextWrapped("路径: %s", m.missingTexturePath.c_str());
                    break;
                case MaterialFallbackReason::NoBaseColor:
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                        u8"无 base_color 也无贴图，使用派生色");
                    break;
                }
                ImGui::ColorEdit3(u8"纯色",
                    &const_cast<MaterialGPU&>(m).baseColor.x,
                    ImGuiColorEditFlags_NoAlpha);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader(u8"统计"))
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("FPS: %.1f", io.Framerate);
        ImGui::Text(u8"离屏 RT: %d x %d", g_r.offW, g_r.offH);
        ImGui::Text(u8"索引数: %u", g_r.indexCount);
        ImGui::Text(u8"三角形: %u", g_r.indexCount / 3);
    }

    ImGui::End();
}

static void DrawUI()
{
    DrawMainMenuBar();

    const float menuH = ImGui::GetFrameHeight();
    const float winW = (float)g_r.width;
    const float winH = (float)g_r.height;
    const float panelW = 340.0f;

    if (g_r.showPanel)
    {
        ImGui::SetNextWindowPos(ImVec2(0.0f, menuH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelW, winH - menuH), ImGuiCond_Always);
        DrawSidePanel();
    }

    const float viewX = g_r.showPanel ? panelW : 0.0f;
    const float viewW = winW - viewX;
    if (viewW > 1.0f && winH - menuH > 1.0f)
    {
        ImGui::SetNextWindowPos(ImVec2(viewX, menuH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(viewW, winH - menuH), ImGuiCond_Always);
        DrawViewportWindow();
    }
}

// ============================================================
// 12. Win32 窗口
// ============================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && g_r.device)
        {
            g_r.width = LOWORD(lParam);
            g_r.height = HIWORD(lParam);
            if (g_r.width > 0 && g_r.height > 0)
            {
                g_r.backRTV.Reset();
                g_r.backDSV.Reset();
                g_r.backDepthTex.Reset();

                HRESULT hr = g_r.swapChain->ResizeBuffers(
                    0, g_r.width, g_r.height, DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(hr))
                    CreateBackBuffer();
            }
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================
// 13. 入口
// ============================================================
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FBXViewerWndClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Model Viewer",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1440, 900,
        nullptr, nullptr, hInst, nullptr);

    g_r.hwnd = hwnd;
    g_r.width = 1440;
    g_r.height = 900;

    if (!CreateDeviceAndSwapChain())
    {
        MessageBoxW(hwnd, L"D3D11 设备创建失败", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }
    CreateBackBuffer();

    if (!CreatePipeline())
    {
        MessageBoxW(hwnd, L"着色器编译失败", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    LoadDefaultSphere();

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.AntiAliasedLines = false;
    style.AntiAliasedFill = false;

    // 中文字体（找不到就跳过）
    {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        const char* fontPaths[] = {
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/msyhbd.ttc",
            "C:/Windows/Fonts/simhei.ttf",
            "C:/Windows/Fonts/simsun.ttc",
        };
        for (const char* p : fontPaths)
        {
            if (io.Fonts->AddFontFromFileTTF(p, 18.0f, &cfg,
                io.Fonts->GetGlyphRangesChineseFull()))
                break;
        }
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_r.device.Get(), g_r.ctx.Get());

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // ---- 主循环 ----
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    while (g_running)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }

        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - prev.QuadPart) / (float)freq.QuadPart;
        prev = now;
        dt = std::min(dt, 1.0f / 15.0f);

        if (g_r.autoRotate)
            g_r.modelRotationY += dt * 0.6f;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUI();

        // 处理文件加载
        if (g_r.hasPendingFile)
        {
            g_r.hasPendingFile = false;
            if (!LoadModel(g_r.pendingFile))
            {
                MessageBoxW(g_r.hwnd,
                    L"模型加载失败，详情见右侧控制面板顶部状态栏。",
                    L"错误", MB_OK | MB_ICONWARNING);
            }
        }

        ImGui::Render();

        const float bgColor[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
        g_r.ctx->OMSetRenderTargets(1, g_r.backRTV.GetAddressOf(),
            g_r.backDSV.Get());
        g_r.ctx->ClearRenderTargetView(g_r.backRTV.Get(), bgColor);

        D3D11_VIEWPORT vp = {};
        vp.Width = (float)g_r.width;
        vp.Height = (float)g_r.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        g_r.ctx->RSSetViewports(1, &vp);

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_r.swapChain->Present(1, 0);
    }

    // ---- 清理 ----
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_r.textures.clear();
    g_r.materials.clear();
    g_r.drawCalls.clear();
    g_r.vb.Reset(); g_r.ib.Reset();
    g_r.cbPerFrame.Reset(); g_r.cbPerObject.Reset();
    g_r.vs.Reset(); g_r.ps.Reset(); g_r.inputLayout.Reset();
    g_r.rsSolid.Reset(); g_r.rsWire.Reset();
    g_r.dsState.Reset(); g_r.sampler.Reset();
    g_r.offColorTex.Reset(); g_r.offRTV.Reset(); g_r.offSRV.Reset();
    g_r.offDepthTex.Reset(); g_r.offDSV.Reset();
    g_r.backRTV.Reset(); g_r.backDSV.Reset(); g_r.backDepthTex.Reset();
    g_r.swapChain.Reset(); g_r.ctx.Reset(); g_r.device.Reset();

    if (g_wicFactory) { g_wicFactory->Release(); g_wicFactory = nullptr; }

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
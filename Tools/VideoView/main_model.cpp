#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cstdint>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ============================================================
//  1. 着色器源代码（内嵌）
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
};

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
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    float4 worldPos = mul(float4(input.pos, 1.0f), gWorld);
    o.worldPos = worldPos.xyz;
    o.pos      = mul(worldPos, gViewProj);
    o.normal   = normalize(mul(input.normal, (float3x3)gWorld));
    return o;
}

float4 PSMain(PSInput input) : SV_TARGET
{
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

    float3 color = gColor.rgb * (ambient + diff) + spec * 0.6f;
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
};

struct Camera
{
    XMFLOAT3 target = { 0.0f, 0.0f, 0.0f };
    float    distance = 4.0f;
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
        return XMMatrixLookAtLH(
            XMLoadFloat3(&eye),
            XMLoadFloat3(&target),
            XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
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
        distance = std::max(0.3f, std::min(200.0f, distance));
    }
};

enum class ModelType { Sphere = 0, Cube = 1, Torus = 2 };

// ============================================================
//  3. 全局渲染器状态
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

    // 离屏 RT
    ComPtr<ID3D11Texture2D>          offColorTex;
    ComPtr<ID3D11RenderTargetView>   offRTV;
    ComPtr<ID3D11ShaderResourceView> offSRV;
    ComPtr<ID3D11Texture2D>          offDepthTex;
    ComPtr<ID3D11DepthStencilView>   offDSV;
    int offW = 0;
    int offH = 0;

    // 管线
    ComPtr<ID3D11VertexShader>    vs;
    ComPtr<ID3D11PixelShader>     ps;
    ComPtr<ID3D11InputLayout>     inputLayout;
    ComPtr<ID3D11Buffer>          cbPerFrame;
    ComPtr<ID3D11Buffer>          cbPerObject;
    ComPtr<ID3D11RasterizerState> rsState;
    ComPtr<ID3D11DepthStencilState> dsState;

    // 网格
    ComPtr<ID3D11Buffer> vb;
    ComPtr<ID3D11Buffer> ib;
    UINT indexCount = 0;

    // 状态
    Camera    camera;
    ModelType currentModel = ModelType::Sphere;
    float     modelRotationY = 0.0f;
    bool      autoRotate = true;
    float     lightYaw = 0.6f;
    float     lightPitch = 0.8f;
    XMFLOAT4  modelColor = XMFLOAT4(0.85f, 0.55f, 0.25f, 1.0f);
    bool      showDemoWindow = false;
    bool      showPanel = true;
    bool      wireframe = false;

    // 布局初始化标志
    bool      layoutInit = false;
};

static Renderer g_r;
static bool     g_running = true;

// ============================================================
//  4. 网格生成
// ============================================================
static void GenerateSphere(std::vector<Vertex>& outV,
    std::vector<uint32_t>& outI,
    float radius = 1.0f,
    int segments = 48,
    int rings = 32)
{
    outV.clear(); outI.clear();
    for (int y = 0; y <= rings; ++y)
    {
        float v = (float)y / (float)rings;
        float phi = v * XM_PI;
        for (int x = 0; x <= segments; ++x)
        {
            float u = (float)x / (float)segments;
            float theta = u * XM_2PI;

            float px = radius * sinf(phi) * cosf(theta);
            float py = radius * cosf(phi);
            float pz = radius * sinf(phi) * sinf(theta);

            Vertex vert;
            vert.pos = XMFLOAT3(px, py, pz);
            vert.normal = XMFLOAT3(px / radius, py / radius, pz / radius);
            vert.uv = XMFLOAT2(u, v);
            outV.push_back(vert);
        }
    }
    for (int y = 0; y < rings; ++y)
    {
        for (int x = 0; x < segments; ++x)
        {
            uint32_t a = (uint32_t)(y * (segments + 1) + x);
            uint32_t b = a + 1;
            uint32_t c = (uint32_t)((y + 1) * (segments + 1) + x);
            uint32_t d = c + 1;
            outI.push_back(a); outI.push_back(c); outI.push_back(b);
            outI.push_back(b); outI.push_back(c); outI.push_back(d);
        }
    }
}

static void GenerateCube(std::vector<Vertex>& outV,
    std::vector<uint32_t>& outI,
    float halfSize = 1.0f)
{
    outV.clear(); outI.clear();

    const XMFLOAT3 faceNormals[6] = {
        {  1,  0,  0 }, { -1,  0,  0 },
        {  0,  1,  0 }, {  0, -1,  0 },
        {  0,  0,  1 }, {  0,  0, -1 },
    };
    const XMFLOAT3 faceVerts[6][4] = {
        { {  1, -1, -1 }, {  1,  1, -1 }, {  1,  1,  1 }, {  1, -1,  1 } },
        { { -1, -1,  1 }, { -1,  1,  1 }, { -1,  1, -1 }, { -1, -1, -1 } },
        { { -1,  1, -1 }, { -1,  1,  1 }, {  1,  1,  1 }, {  1,  1, -1 } },
        { { -1, -1,  1 }, { -1, -1, -1 }, {  1, -1, -1 }, {  1, -1,  1 } },
        { { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 } },
        { {  1, -1, -1 }, { -1, -1, -1 }, { -1,  1, -1 }, {  1,  1, -1 } },
    };

    for (int f = 0; f < 6; ++f)
    {
        uint32_t base = (uint32_t)outV.size();
        for (int i = 0; i < 4; ++i)
        {
            Vertex v;
            v.pos = XMFLOAT3(faceVerts[f][i].x * halfSize,
                faceVerts[f][i].y * halfSize,
                faceVerts[f][i].z * halfSize);
            v.normal = faceNormals[f];
            v.uv = XMFLOAT2((i == 1 || i == 2) ? 1.0f : 0.0f,
                (i >= 2) ? 1.0f : 0.0f);
            outV.push_back(v);
        }
        outI.push_back(base + 0); outI.push_back(base + 1); outI.push_back(base + 2);
        outI.push_back(base + 0); outI.push_back(base + 2); outI.push_back(base + 3);
    }
}

static void GenerateTorus(std::vector<Vertex>& outV,
    std::vector<uint32_t>& outI,
    float majorR = 1.0f,
    float minorR = 0.35f,
    int   majorSeg = 64,
    int   minorSeg = 24)
{
    outV.clear(); outI.clear();
    for (int i = 0; i <= majorSeg; ++i)
    {
        float u = (float)i / (float)majorSeg;
        float theta = u * XM_2PI;
        float ct = cosf(theta), st = sinf(theta);

        for (int j = 0; j <= minorSeg; ++j)
        {
            float v = (float)j / (float)minorSeg;
            float phi = v * XM_2PI;
            float cp = cosf(phi), sp = sinf(phi);

            float x = (majorR + minorR * cp) * ct;
            float y = minorR * sp;
            float z = (majorR + minorR * cp) * st;

            Vertex vert;
            vert.pos = XMFLOAT3(x, y, z);
            vert.normal = XMFLOAT3(cp * ct, sp, cp * st);
            vert.uv = XMFLOAT2(u, v);
            outV.push_back(vert);
        }
    }
    for (int i = 0; i < majorSeg; ++i)
    {
        for (int j = 0; j < minorSeg; ++j)
        {
            uint32_t a = (uint32_t)(i * (minorSeg + 1) + j);
            uint32_t b = a + 1;
            uint32_t c = (uint32_t)((i + 1) * (minorSeg + 1) + j);
            uint32_t d = c + 1;
            outI.push_back(a); outI.push_back(c); outI.push_back(b);
            outI.push_back(b); outI.push_back(c); outI.push_back(d);
        }
    }
}

// ============================================================
//  5. D3D11 初始化
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
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
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
    g_r.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer)
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
    g_r.device->CreateTexture2D(&desc, nullptr, &g_r.backDepthTex);
    if (g_r.backDepthTex)
        g_r.device->CreateDepthStencilView(g_r.backDepthTex.Get(), nullptr, &g_r.backDSV);
}

static void ResizeOffscreen(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (w == g_r.offW && h == g_r.offH) return;

    g_r.offRTV.Reset();
    g_r.offSRV.Reset();
    g_r.offColorTex.Reset();
    g_r.offDSV.Reset();
    g_r.offDepthTex.Reset();

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
        nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", flags, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) OutputDebugStringA((const char*)errBlob->GetBufferPointer());
        return false;
    }
    hr = D3DCompile(g_shaderSource, strlen(g_shaderSource),
        nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", flags, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
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
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        &g_r.inputLayout);

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
    dev->CreateRasterizerState(&rs, &g_r.rsState);

    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    ds.StencilEnable = FALSE;
    dev->CreateDepthStencilState(&ds, &g_r.dsState);

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
}

static void LoadModel(ModelType type)
{
    std::vector<Vertex>   verts;
    std::vector<uint32_t> indices;

    switch (type)
    {
    case ModelType::Sphere: GenerateSphere(verts, indices); break;
    case ModelType::Cube:   GenerateCube(verts, indices); break;
    case ModelType::Torus:  GenerateTorus(verts, indices); break;
    }
    UploadMesh(verts, indices);
}

// ============================================================
//  6. 3D 渲染
// ============================================================
static void RenderScene3D()
{
    if (!g_r.offRTV || !g_r.offDSV || g_r.offW <= 0 || g_r.offH <= 0) return;

    ID3D11DeviceContext* dc = g_r.ctx.Get();

    ID3D11RenderTargetView* rtvs[1] = { g_r.offRTV.Get() };
    dc->OMSetRenderTargets(1, rtvs, g_r.offDSV.Get());

    const float clearColor[4] = { 0.10f, 0.11f, 0.14f, 1.0f };
    dc->ClearRenderTargetView(g_r.offRTV.Get(), clearColor);
    dc->ClearDepthStencilView(g_r.offDSV.Get(),
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
        1.0f, 0);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)g_r.offW;
    vp.Height = (float)g_r.offH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    dc->RSSetState(g_r.rsState.Get());
    dc->OMSetDepthStencilState(g_r.dsState.Get(), 0);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(g_r.inputLayout.Get());
    dc->VSSetShader(g_r.vs.Get(), nullptr, 0);
    dc->PSSetShader(g_r.ps.Get(), nullptr, 0);

    PerFrameCB pf;
    XMMATRIX view = g_r.camera.GetView();
    XMMATRIX proj = g_r.camera.GetProj((float)g_r.offW / (float)g_r.offH);
    XMMATRIX vpMat = XMMatrixMultiply(view, proj);
    XMStoreFloat4x4(&pf.viewProj, XMMatrixTranspose(vpMat));

    XMFLOAT3 camPos = g_r.camera.GetPosition();
    pf.cameraPos = XMFLOAT4(camPos.x, camPos.y, camPos.z, 1.0f);

    float lcp = cosf(g_r.lightPitch);
    XMFLOAT3 lightDir = {
        -lcp * sinf(g_r.lightYaw),
        -sinf(g_r.lightPitch),
        -lcp * cosf(g_r.lightYaw)
    };
    pf.lightDir = XMFLOAT4(lightDir.x, lightDir.y, lightDir.z, 0.0f);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(dc->Map(g_r.cbPerFrame.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &pf, sizeof(pf));
        dc->Unmap(g_r.cbPerFrame.Get(), 0);
    }
    dc->VSSetConstantBuffers(0, 1, g_r.cbPerFrame.GetAddressOf());
    dc->PSSetConstantBuffers(0, 1, g_r.cbPerFrame.GetAddressOf());

    PerObjectCB po;
    XMMATRIX world = XMMatrixRotationY(g_r.modelRotationY);
    XMStoreFloat4x4(&po.world, XMMatrixTranspose(world));
    po.color = g_r.modelColor;

    if (SUCCEEDED(dc->Map(g_r.cbPerObject.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &po, sizeof(po));
        dc->Unmap(g_r.cbPerObject.Get(), 0);
    }
    dc->VSSetConstantBuffers(1, 1, g_r.cbPerObject.GetAddressOf());
    dc->PSSetConstantBuffers(1, 1, g_r.cbPerObject.GetAddressOf());

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vbs[1] = { g_r.vb.Get() };
    dc->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    dc->IASetIndexBuffer(g_r.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    dc->DrawIndexed(g_r.indexCount, 0, 0);

    dc->OMSetRenderTargets(0, nullptr, nullptr);
    dc->PSSetShaderResources(0, 0, nullptr);
}

// ============================================================
//  7. ImGui UI
// ============================================================
static void DrawMainMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("文件"))
        {
            if (ImGui::MenuItem("退出", "Alt+F4")) g_running = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图"))
        {
            ImGui::MenuItem("控制面板", nullptr, &g_r.showPanel);
            ImGui::MenuItem("ImGui 演示", nullptr, &g_r.showDemoWindow);
            ImGui::MenuItem("线框模式", nullptr, &g_r.wireframe);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("模型"))
        {
            if (ImGui::MenuItem("球体", nullptr, g_r.currentModel == ModelType::Sphere)) {
                g_r.currentModel = ModelType::Sphere; LoadModel(ModelType::Sphere);
            }
            if (ImGui::MenuItem("立方体", nullptr, g_r.currentModel == ModelType::Cube)) {
                g_r.currentModel = ModelType::Cube;   LoadModel(ModelType::Cube);
            }
            if (ImGui::MenuItem("圆环", nullptr, g_r.currentModel == ModelType::Torus)) {
                g_r.currentModel = ModelType::Torus;  LoadModel(ModelType::Torus);
            }
            ImGui::EndMenu();
        }

        ImGuiIO& io = ImGui::GetIO();
        char fps[64];
        snprintf(fps, sizeof(fps), "%.1f FPS (%.2f ms)",
            io.Framerate, 1000.0f / io.Framerate);
        float w = ImGui::CalcTextSize(fps).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - w - 20);
        ImGui::TextUnformatted(fps);

        ImGui::EndMainMenuBar();
    }
}

static void HandleViewportInput(bool hovered)
{
    if (!hovered) return;

    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        g_r.camera.yaw -= io.MouseDelta.x * 0.008f;
        g_r.camera.pitch += io.MouseDelta.y * 0.008f;
        g_r.camera.Clamp();
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
        ImGui::IsMouseDragging(ImGuiMouseButton_Right))
    {
        XMVECTOR right = XMVectorSet(cosf(g_r.camera.yaw), 0.0f, -sinf(g_r.camera.yaw), 0.0f);
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        float    s = g_r.camera.distance * 0.0015f;

        XMVECTOR delta = XMVectorScale(right, -io.MouseDelta.x * s);
        delta = XMVectorAdd(delta, XMVectorScale(up, io.MouseDelta.y * s));

        XMFLOAT3 d;
        XMStoreFloat3(&d, delta);
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
    ImGui::Begin("视口", nullptr,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

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
        "左键拖动旋转 | 中/右键平移 | 滚轮缩放");

    ImGui::End();
    ImGui::PopStyleVar();
}

static void DrawSidePanel()
{
    if (!g_r.showPanel) return;

    ImGui::Begin("控制面板", &g_r.showPanel);

    if (ImGui::CollapsingHeader("模型", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int modelIdx = (int)g_r.currentModel;
        const char* modelNames[] = { "球体", "立方体", "圆环" };
        if (ImGui::Combo("当前模型", &modelIdx, modelNames, 3))
        {
            g_r.currentModel = (ModelType)modelIdx;
            LoadModel(g_r.currentModel);
        }

        ImGui::Checkbox("自动旋转", &g_r.autoRotate);
        ImGui::SliderFloat("旋转 Y", &g_r.modelRotationY, -XM_PI, XM_PI, "%.3f rad");

        float col[4] = { g_r.modelColor.x, g_r.modelColor.y, g_r.modelColor.z, g_r.modelColor.w };
        if (ImGui::ColorEdit4("模型颜色", col))
            g_r.modelColor = XMFLOAT4(col[0], col[1], col[2], col[3]);
    }

    if (ImGui::CollapsingHeader("相机", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("距离", &g_r.camera.distance, 0.3f, 50.0f, "%.2f");
        ImGui::SliderFloat("Yaw", &g_r.camera.yaw, -XM_PI, XM_PI, "%.2f rad");
        ImGui::SliderFloat("Pitch", &g_r.camera.pitch, -1.5f, 1.5f, "%.2f rad");
        ImGui::SliderFloat("FOV", &g_r.camera.fovY, 20.0f, 120.0f, "%.0f deg");
        if (ImGui::Button("重置相机"))
            g_r.camera = Camera();
    }

    if (ImGui::CollapsingHeader("光照", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("光 Yaw", &g_r.lightYaw, -XM_PI, XM_PI, "%.2f");
        ImGui::SliderFloat("光 Pitch", &g_r.lightPitch, -1.5f, 1.5f, "%.2f");
    }

    if (ImGui::CollapsingHeader("统计"))
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("FPS: %.1f", io.Framerate);
        ImGui::Text("Frame: %.3f ms", io.DeltaTime * 1000.0f);
        ImGui::Text("离屏 RT: %d x %d", g_r.offW, g_r.offH);
        ImGui::Text("索引数: %u", g_r.indexCount);
        ImGui::Text("三角形: %u", g_r.indexCount / 3);
    }

    ImGui::End();
}

static void DrawUI()
{
    DrawMainMenuBar();

    // 首次运行时给两张窗口一个合理的初始位置和大小
    if (!g_r.layoutInit)
    {
        g_r.layoutInit = true;

        const float menuH = ImGui::GetFrameHeight();
        const float panelW = 320.0f;
        const float winW = (float)g_r.width;
        const float winH = (float)g_r.height;

        // 控制面板：左侧
        ImGui::SetNextWindowPos(ImVec2(0.0f, menuH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelW, winH - menuH), ImGuiCond_Always);

        // 视口：右侧剩余空间
        ImGui::SetNextWindowPos(ImVec2(panelW, menuH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(winW - panelW, winH - menuH), ImGuiCond_Always);
    }

    // 控制面板先画（在左侧），视口后画
    DrawSidePanel();
    DrawViewportWindow();

    if (g_r.showDemoWindow)
        ImGui::ShowDemoWindow(&g_r.showDemoWindow);
}

// ============================================================
//  8. Win32 窗口
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
                g_r.swapChain->ResizeBuffers(0, g_r.width, g_r.height,
                    DXGI_FORMAT_UNKNOWN, 0);
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
//  9. 主入口
// ============================================================
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ImGui3DViewerWndClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ImGui 3D Model Viewer",
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

    LoadModel(g_r.currentModel);

    // ------------- ImGui 初始化 -------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 注意：不再启用 DockingEnable / ViewportsEnable

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.AntiAliasedLines = false;
    style.AntiAliasedFill = false;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_r.device.Get(), g_r.ctx.Get());

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // ------------- 主循环 -------------
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

        if (g_r.autoRotate)
            g_r.modelRotationY += dt * 0.6f;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUI();

        ImGui::Render();

        const float bgColor[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
        g_r.ctx->OMSetRenderTargets(1, g_r.backRTV.GetAddressOf(), g_r.backDSV.Get());
        g_r.ctx->ClearRenderTargetView(g_r.backRTV.Get(), bgColor);

        D3D11_VIEWPORT vp = {};
        vp.Width = (float)g_r.width;
        vp.Height = (float)g_r.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        g_r.ctx->RSSetViewports(1, &vp);

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_r.swapChain->Present(1, 0); // VSync
    }

    // ------------- 清理 -------------
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_r.vb.Reset(); g_r.ib.Reset();
    g_r.cbPerFrame.Reset(); g_r.cbPerObject.Reset();
    g_r.vs.Reset(); g_r.ps.Reset(); g_r.inputLayout.Reset();
    g_r.rsState.Reset(); g_r.dsState.Reset();
    g_r.offColorTex.Reset(); g_r.offRTV.Reset(); g_r.offSRV.Reset();
    g_r.offDepthTex.Reset(); g_r.offDSV.Reset();
    g_r.backRTV.Reset(); g_r.backDSV.Reset(); g_r.backDepthTex.Reset();
    g_r.swapChain.Reset(); g_r.ctx.Reset(); g_r.device.Reset();

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
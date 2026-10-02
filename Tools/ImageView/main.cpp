#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>

#include "stb_image.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

// ==================== DX11 全局对象 ====================
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// ---- 采样器状态（用于像素模式） ----
static ID3D11SamplerState* g_pSamplerPoint = nullptr;   // 点采样（最近邻）
static ID3D11SamplerState* g_pSamplerLinear = nullptr;  // 线性采样

// ==================== 图像数据结构 ====================
struct ImageData
{
    int width = 0;
    int height = 0;
    int channels = 0;

    // 路径，需要时重新解码
    std::wstring sourcePath;

    ID3D11Texture2D* texRGBA = nullptr;
    ID3D11ShaderResourceView* srvRGBA = nullptr;

    ID3D11ShaderResourceView* srvR = nullptr;
    ID3D11ShaderResourceView* srvG = nullptr;
    ID3D11ShaderResourceView* srvB = nullptr;
    ID3D11ShaderResourceView* srvA = nullptr;

    bool channelFailed[4] = { false, false, false, false };

    void ReleaseChannelSRVs()
    {
        if (srvR) { srvR->Release(); srvR = nullptr; }
        if (srvG) { srvG->Release(); srvG = nullptr; }
        if (srvB) { srvB->Release(); srvB = nullptr; }
        if (srvA) { srvA->Release(); srvA = nullptr; }
        // 清空标记，允许下次重新尝试
        channelFailed[0] = channelFailed[1] =
            channelFailed[2] = channelFailed[3] = false;
    }

    void Release()
    {
        ReleaseChannelSRVs();
        if (srvRGBA) { srvRGBA->Release(); srvRGBA = nullptr; }
        if (texRGBA) { texRGBA->Release(); texRGBA = nullptr; }
        sourcePath.clear();
        width = height = channels = 0;
    }
};

// ==================== 通道查看状态 ====================
struct ChannelView
{
    bool showR = true;
    bool showG = true;
    bool showB = true;
    bool showA = true;
    bool separateMode = false;
    bool fitToWindow = true;
    bool pixelPerfect = false;   // 像素完美模式（点采样）

    // 视图变换
    float  zoom = 1.0f;
    ImVec2 pan = ImVec2(0.0f, 0.0f);   // 相对内容区中心的偏移

    void ResetView()
    {
        fitToWindow = true;
        zoom = 1.0f;
        pan = ImVec2(0.0f, 0.0f);
    }
};

// ==================== 全局应用状态 ====================
static ImageData   g_image;
static ChannelView g_channelView;
static std::string g_statusMessage;
static bool        g_needRedraw = true;

static std::vector<unsigned char> g_channelScratch;

// ==================== 前向声明 ====================
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ==================== 采样器创建 ====================
bool CreateSamplers()
{
    // 点采样：像素画专用
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(g_pd3dDevice->CreateSamplerState(&sd, &g_pSamplerPoint)))
            return false;
    }
    // 线性采样：恢复用（与 ImGui 后端默认设置保持一致）
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(g_pd3dDevice->CreateSamplerState(&sd, &g_pSamplerLinear)))
            return false;
    }
    return true;
}

// ==================== 绘制回调：切换采样器 ====================
// 在 Image() 之前调用，切到点采样
static void SetPointSamplerCb(const ImDrawList*, const ImDrawCmd*)
{
    g_pd3dDeviceContext->PSSetSamplers(0, 1, &g_pSamplerPoint);
}

// 在 Image() 之后调用，恢复线性采样
static void SetLinearSamplerCb(const ImDrawList*, const ImDrawCmd*)
{
    g_pd3dDeviceContext->PSSetSamplers(0, 1, &g_pSamplerLinear);
}

// ==================== 带采样模式的 Image 封装 ====================
static void DrawImageWithSampling(ImTextureID tex, ImVec2 size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool point = g_channelView.pixelPerfect;

    if (point)
        dl->AddCallback(SetPointSamplerCb, nullptr);

    ImGui::Image(tex, size);

    if (point)
        dl->AddCallback(SetLinearSamplerCb, nullptr);
}

// ==================== DX11 纹理创建 ====================
static bool ReadFileToBuffer(const std::wstring& path,
    std::vector<unsigned char>& outBuf)
{
    HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!::GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 512ll * 1024 * 1024)
    {
        ::CloseHandle(hFile);
        return false;
    }

    outBuf.resize((size_t)size.QuadPart);
    DWORD bytesRead = 0;
    BOOL ok = ::ReadFile(hFile, outBuf.data(),
        (DWORD)outBuf.size(), &bytesRead, nullptr);
    ::CloseHandle(hFile);
    return ok && bytesRead == (DWORD)outBuf.size();
}

bool CreateTextureFromPixels(
    ID3D11Device* device,
    const unsigned char* pixels, int width, int height, int stride,
    DXGI_FORMAT format,
    ID3D11Texture2D** outTex,
    ID3D11ShaderResourceView** outSRV)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels;
    initData.SysMemPitch = stride;

    if (FAILED(device->CreateTexture2D(&desc, &initData, outTex)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    if (FAILED(device->CreateShaderResourceView(*outTex, &srvDesc, outSRV)))
    {
        (*outTex)->Release();
        *outTex = nullptr;
        return false;
    }
    return true;
}

bool CreateRGBATexture(ImageData& img,
    const unsigned char* rgbaPixels)
{
    if (img.texRGBA) { img.texRGBA->Release(); img.texRGBA = nullptr; }
    if (img.srvRGBA) { img.srvRGBA->Release(); img.srvRGBA = nullptr; }

    return CreateTextureFromPixels(
        g_pd3dDevice, rgbaPixels, img.width, img.height, img.width * 4,
        DXGI_FORMAT_R8G8B8A8_UNORM, &img.texRGBA, &img.srvRGBA);
}

bool CreateChannelTexture(
    ID3D11Device* device,
    const unsigned char* rgbaPixels, int width, int height,
    int channelIndex,
    ID3D11ShaderResourceView** outSRV)
{
    const int    pixelCount = width * height;
    const size_t need = (size_t)pixelCount * 4;

    // 复用全局缓冲：只在需要更大时扩容，不缩小
    if (g_channelScratch.size() < need)
        g_channelScratch.resize(need);

    unsigned char* dst = g_channelScratch.data();

    // 一次写 4 字节，让编译器向量化
    uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
    for (int i = 0; i < pixelCount; ++i)
    {
        unsigned char v = rgbaPixels[i * 4 + channelIndex];
        dst32[i] = (uint32_t)v
            | ((uint32_t)v << 8)
            | ((uint32_t)v << 16)
            | (0xFFu << 24);   // A = 255
    }

    ID3D11Texture2D* tex = nullptr;
    bool ok = CreateTextureFromPixels(
        device, dst, width, height, width * 4,
        DXGI_FORMAT_R8G8B8A8_UNORM, &tex, outSRV);

    if (tex) tex->Release();
    return ok;
}
// ==================== 按需更新通道纹理 ====================
void UpdateChannelTextures(ImageData& img, const ChannelView& view)
{
    // 离开分离模式不释放 SRV，保留到换图
    if (!view.separateMode) return;

    if (img.sourcePath.empty()) return;

    // 判断是否真的需要创建新通道
    auto need = [&](bool show, ID3D11ShaderResourceView* srv, bool failed)
        {
            return show && !srv && !failed;
        };
    if (!need(view.showR, img.srvR, img.channelFailed[0]) &&
        !need(view.showG, img.srvG, img.channelFailed[1]) &&
        !need(view.showB, img.srvB, img.channelFailed[2]) &&
        !need(view.showA, img.srvA, img.channelFailed[3]))
        return;

    // 按需重新解码
    std::vector<unsigned char> buffer;
    if (!ReadFileToBuffer(img.sourcePath, buffer))
    {
        g_statusMessage = u8"读取文件失败（通道分离）";
        return;
    }

    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load_from_memory(
        buffer.data(), (int)buffer.size(), &w, &h, &ch, 4);
    if (!pixels)
    {
        g_statusMessage = u8"解码失败（通道分离）";
        return;
    }

    auto ensure = [&](bool show, int idx, ID3D11ShaderResourceView** srv)
        {
            if (!show || *srv || img.channelFailed[idx]) return;
            if (!CreateChannelTexture(g_pd3dDevice, pixels,
                img.width, img.height, idx, srv))
            {
                img.channelFailed[idx] = true;
                g_statusMessage = u8"通道纹理创建失败";
            }
        };

    ensure(view.showR, 0, &img.srvR);
    ensure(view.showG, 1, &img.srvG);
    ensure(view.showB, 2, &img.srvB);
    ensure(view.showA, 3, &img.srvA);

    stbi_image_free(pixels);
}

// ==================== 图像加载 ====================
bool LoadImageFromFileW(const wchar_t* pathW, ImageData& outImg)
{
    outImg.Release();

    std::vector<unsigned char> buffer;
    if (!ReadFileToBuffer(pathW, buffer)) return false;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        buffer.data(), (int)buffer.size(), &w, &h, &channels, 4);
    if (!pixels) return false;

    outImg.width = w;
    outImg.height = h;
    outImg.channels = channels;
    outImg.sourcePath = pathW;   // ← 记住路径，供通道分离懒加载

    const bool ok = CreateRGBATexture(outImg, pixels);

    // 立刻释放，不再常驻内存
    stbi_image_free(pixels);

    if (!ok)
    {
        outImg.Release();
        return false;
    }
    return true;
}

// ==================== 文件对话框 ====================
bool OpenFileDialogW(std::wstring& outPath)
{
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter =
        L"Image Files\0"
        L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif;*.psd;*.hdr;*.pic;*.pnm;*.webp\0"
        L"All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (::GetOpenFileNameW(&ofn))
    {
        outPath = filename;
        return true;
    }
    return false;
}

// ==================== 打开文件（统一入口） ====================
static void OnImageLoaded()
{
    g_channelView.separateMode = false;
    g_channelView.ResetView();
    g_image.ReleaseChannelSRVs();
    g_statusMessage.clear();

    // 按新图尺寸收缩通道缓冲
    if (g_channelScratch.capacity() >
        (size_t)g_image.width * g_image.height * 4 * 8)
    {
        std::vector<unsigned char>().swap(g_channelScratch);
    }
}

// 把宽字符路径转 UTF-8，用于拼错误消息
static std::string WideToUtf8(const wchar_t* w)
{
    if (!w) return {};

    int need = ::WideCharToMultiByte(
        CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};

    std::string out(need, '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, w, -1,
        &out[0], 
        need, nullptr, nullptr);
    out.resize(need - 1); 
    return out;
}



bool OpenImageFromPath(const std::wstring& path)
{
    if (LoadImageFromFileW(path.c_str(), g_image))
    {
        OnImageLoaded();
        return true;
    }
    g_statusMessage = u8"无法加载文件: " + WideToUtf8(path.c_str());
    return false;
}

// ==================== 工具栏绘制辅助 ====================
static void ToolbarSeparator()
{
    ImGui::SameLine(0, 12.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(p.x, p.y + 4),
        ImVec2(p.x, p.y + h - 4),
        ImGui::GetColorU32(ImVec4(0.38f, 0.38f, 0.42f, 1.0f)), 1.0f);
    ImGui::Dummy(ImVec2(1.0f, h));
    ImGui::SameLine(0, 12.0f);
}

// ==================== UI：顶部固定工具栏 ====================
void DrawToolbar(ChannelView& view, ImageData& img, float toolbarHeight)
{
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, toolbarHeight));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.16f, 0.16f, 0.18f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 7.0f));

    ImGui::Begin("##Toolbar", nullptr, flags);
    {
        if (ImGui::Button(u8"打开图片", ImVec2(90.0f, 0.0f)))
        {
            std::wstring path;
            if (OpenFileDialogW(path))
                OpenImageFromPath(path);
        }

        ImGui::SameLine(0, 8.0f);
        ImGui::Checkbox(u8"适应窗口", &view.fitToWindow);

        // ★ 像素完美模式
        ImGui::SameLine(0, 8.0f);
        ImGui::Checkbox(u8"像素", &view.pixelPerfect);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"以最近邻采样绘制图像，避免缩放时像素画变模糊");

        ToolbarSeparator();

        ImGui::TextUnformatted(u8"通道:");
        ImGui::SameLine(0, 8.0f);
        ImGui::Checkbox("R", &view.showR);
        ImGui::SameLine(0, 8.0f);
        ImGui::Checkbox("G", &view.showG);
        ImGui::SameLine(0, 8.0f);
        ImGui::Checkbox("B", &view.showB);
        ImGui::SameLine(0, 8.0f);
        ImGui::Checkbox("A", &view.showA);

        ImGui::SameLine(0, 16.0f);
        ImGui::Checkbox(u8"通道分离模式", &view.separateMode);

        if (&view.separateMode)
        {
            char buf[128];
            sprintf_s(buf, u8"%d x %d   通道: %d", img.width, img.height, img.channels);
            float textW = ImGui::CalcTextSize(buf).x;
            float targetX = ImGui::GetWindowWidth() - textW - 16.0f;

            ImGui::SameLine();
            if (targetX > ImGui::GetCursorPosX())
            {
                ImGui::SetCursorPosX(targetX);
                ImGui::TextDisabled("%s", buf);
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}
// ==================== 单图视图（缩放 / 平移） ====================
static void DrawSingleImageView(const ImageData& img, ChannelView& view)
{
    ImGuiIO& io = ImGui::GetIO();

    ImVec2 contentPos = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x <= 1.0f || avail.y <= 1.0f) return;

    const float paneCX = contentPos.x + avail.x * 0.5f;
    const float paneCY = contentPos.y + avail.y * 0.5f;

    // ---- 适应窗口：每帧重算缩放 ----
    if (view.fitToWindow)
    {
        float sx = avail.x / (float)img.width;
        float sy = avail.y / (float)img.height;
        float z = (sx < sy) ? sx : sy;
        if (view.pixelPerfect) // 像素模式
            z = (z >= 1.0f) ? floorf(z) : 1.0f / ceilf(1.0f / z);
        view.zoom = z;
        view.pan = ImVec2(0.0f, 0.0f);
    }

    // 由当前 zoom/pan 计算图像左上角与显示尺寸
    auto layout = [&](ImVec2& outTL, ImVec2& outSize)
        {
            float w = img.width * view.zoom;
            float h = img.height * view.zoom;
            outSize = ImVec2(w, h);
            outTL = ImVec2(paneCX + view.pan.x - w * 0.5f,
                paneCY + view.pan.y - h * 0.5f);
        };

    // ---- 交互层：覆盖内容区的隐形按钮 ----
    ImGui::SetCursorScreenPos(contentPos);
    ImGui::InvisibleButton("##canvas", avail,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);

    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // ---- 滚轮缩放：以鼠标位置为锚点 ----
    if (hovered && io.MouseWheel != 0.0f)
    {
        ImVec2 tl, size;
        layout(tl, size);

        const ImVec2 mp = io.MousePos;
        // 鼠标当前指向的图像坐标（未缩放的像素坐标）
        const float ix = (mp.x - tl.x) / view.zoom;
        const float iy = (mp.y - tl.y) / view.zoom;

        float newZoom = view.zoom * powf(1.15f, io.MouseWheel);
        if (newZoom < 0.02f)  newZoom = 0.02f;
        else if (newZoom > 128.0f) newZoom = 128.0f;

        // 新缩放下的左上角，使得鼠标仍指向 (ix, iy)
        const float nw = img.width * newZoom;
        const float nh = img.height * newZoom;
        const float ntlx = mp.x - ix * newZoom;
        const float ntly = mp.y - iy * newZoom;
        const float ncx = ntlx + nw * 0.5f;
        const float ncy = ntly + nh * 0.5f;

        view.zoom = newZoom;
        view.pan = ImVec2(ncx - paneCX, ncy - paneCY);
        view.fitToWindow = false;
    }

    // ---- 左键 / 中键拖拽平移 ----
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
        ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
    {
        view.pan.x += io.MouseDelta.x;
        view.pan.y += io.MouseDelta.y;
        view.fitToWindow = false;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    // ---- 双击复位 ----
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        view.fitToWindow = true;
        view.pan = ImVec2(0.0f, 0.0f);
    }

    // ---- 最终绘制（在窗口内容区裁剪内） ----
    ImVec2 tl, size;
    layout(tl, size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(contentPos,
        ImVec2(contentPos.x + avail.x, contentPos.y + avail.y),
        true);

    if (view.pixelPerfect)
        dl->AddCallback(SetPointSamplerCb, nullptr);

    dl->AddImage(reinterpret_cast<ImTextureID>(img.srvRGBA),
        tl, ImVec2(tl.x + size.x, tl.y + size.y));

    if (view.pixelPerfect)
        dl->AddCallback(SetLinearSamplerCb, nullptr);

    dl->PopClipRect();
}

// ==================== UI：图像显示区 ====================
void DrawImageArea(const ImageData& img, ChannelView& view, float toolbarHeight)
{
    ImGuiIO& io = ImGui::GetIO();

    ImVec2 pos(0, toolbarHeight);
    ImVec2 size(io.DisplaySize.x, io.DisplaySize.y - toolbarHeight);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar |          // 不显示滚动条
        ImGuiWindowFlags_NoScrollWithMouse;     // 滚轮不再滚动窗口

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.11f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));

    ImGui::Begin("##ImageArea", nullptr, flags);

    if (img.srvRGBA == nullptr)
    {
        const char* line1 = u8"将图片拖入窗口";
        const char* line2 = u8"或点击左上角「打开图片」";
        ImVec2 s1 = ImGui::CalcTextSize(line1);
        ImVec2 s2 = ImGui::CalcTextSize(line2);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float lineH = ImGui::GetTextLineHeightWithSpacing();

        ImGui::SetCursorPos(ImVec2(
            (avail.x - s1.x) * 0.5f,
            (avail.y - lineH * 2.0f) * 0.5f));
        ImGui::TextDisabled("%s", line1);

        ImGui::SetCursorPosX((avail.x - s2.x) * 0.5f);
        ImGui::TextDisabled("%s", line2);

        if (!g_statusMessage.empty())
        {
            ImVec2 s3 = ImGui::CalcTextSize(g_statusMessage.c_str());
            ImGui::SetCursorPosX((avail.x - s3.x) * 0.5f);
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s",
                g_statusMessage.c_str());
        }
    }
    else if (!view.separateMode)
    {
        DrawSingleImageView(img, view);
    }
    else
    {
        std::vector<ID3D11ShaderResourceView*> active;
        std::vector<const char*> labels;
        if (view.showR && img.srvR) { active.push_back(img.srvR); labels.push_back("R"); }
        if (view.showG && img.srvG) { active.push_back(img.srvG); labels.push_back("G"); }
        if (view.showB && img.srvB) { active.push_back(img.srvB); labels.push_back("B"); }
        if (view.showA && img.srvA) { active.push_back(img.srvA); labels.push_back("A"); }

        if (active.empty())
        {
            ImGui::TextDisabled(u8"未选择任何通道");
        }
        else
        {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float labelH = ImGui::GetTextLineHeightWithSpacing();
            float availH = avail.y - labelH - 8.0f;
            float gap = 8.0f;
            float cellW = (avail.x - gap * (active.size() - 1)) / (float)active.size();

            float scale = 1.0f;
            if (view.fitToWindow)
            {
                float sx = cellW / (float)img.width;
                float sy = availH / (float)img.height;
                scale = (sx < sy) ? sx : sy;
                if (view.pixelPerfect && scale > 1.0f)
                    scale = floorf(scale);
            }
            ImVec2 imgSize(img.width * scale, img.height * scale);

            for (size_t i = 0; i < active.size(); ++i)
            {
                if (i > 0) ImGui::SameLine(0, gap);
                ImGui::BeginGroup();
                ImGui::TextDisabled(u8"%s 通道", labels[i]);
                DrawImageWithSampling((ImTextureID)active[i], imgSize);
                ImGui::EndGroup();
            }
        }
    }

    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// ==================== Win32 / DX11 初始化 ====================
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createDeviceFlags, featureLevelArray, 2,
            D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

    if (FAILED(res))
        return false;

    if (!CreateSamplers())
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSamplerPoint) { g_pSamplerPoint->Release();  g_pSamplerPoint = nullptr; }
    if (g_pSamplerLinear) { g_pSamplerLinear->Release(); g_pSamplerLinear = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer)
    {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView)
    {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    g_needRedraw = true;   // 需要重绘

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam),
                (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;

        UINT count = ::DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0)
        {
            wchar_t pathW[MAX_PATH] = {};
            ::DragQueryFileW(hDrop, 0, pathW, MAX_PATH);

            if (LoadImageFromFileW(pathW, g_image))
            {
                g_channelView.separateMode = false;
                g_channelView.ResetView();
                g_image.ReleaseChannelSRVs();
                g_statusMessage.clear();
            }
            else
            {
                char msg8[512];
                ::WideCharToMultiByte(CP_UTF8, 0, pathW, -1, msg8, sizeof(msg8), nullptr, nullptr);
                g_statusMessage = std::string(u8"无法加载文件: ") + msg8;
            }
        }
        ::DragFinish(hDrop);
        return 0;
    }

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ==================== 渲染一帧 ====================
static void RenderFrame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    float toolbarHeight = ImGui::GetFrameHeight()
        + ImGui::GetStyle().WindowPadding.y * 2.0f + 2.0f;

    DrawToolbar(g_channelView, g_image, toolbarHeight);
    UpdateChannelTextures(g_image, g_channelView);
    DrawImageArea(g_image, g_channelView, toolbarHeight);
    

    ImGui::Render();
    const float clear_color[4] = { 0.10f, 0.10f, 0.11f, 1.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_pSwapChain->Present(1, 0);
}

// ==================== 主函数 ====================
int wmain(int argc, wchar_t** argv)
{
    // 调试命令行窗口
#ifdef _DEBUG
    if (::AllocConsole())
    {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif

    // 解析命令行参数
    std::wstring initialPath;
    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != L'\0')
        initialPath = argv[1];

    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"ImageViewerClass", nullptr
    };
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"ImageView",
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800,
        nullptr, nullptr, wc.hInstance, nullptr);

    ::DragAcceptFiles(hwnd, TRUE);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 载入系统默认字体
    io.Fonts->AddFontFromFileTTF("c:/Windows/Fonts/msyh.ttc", 16.0f, NULL, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (io.Fonts == nullptr) {
        io.Fonts->AddFontDefault();
        g_statusMessage = "Error: Unable to find the Chinese font";
    }
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 如果从命令行传入的图片，在这里加载
    if (!initialPath.empty())
    {
        OpenImageFromPath(initialPath);
    }

    RenderFrame();
    RenderFrame();

    bool done = false;
    while (!done)
    {
        // 抽干消息队列
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) { done = true; break; }
        }
        if (done) break;

        if (g_needRedraw)
        {
            g_needRedraw = false;
            RenderFrame();
        }
        else
        {
            // 无消息时挂起线程
            // QS_ALLINPUT 任何输入消息到达就唤醒
            ::MsgWaitForMultipleObjectsEx(
                0, nullptr,
                INFINITE,           // 一直等，直到有消息
                QS_ALLINPUT,
                0);
        }
    }

    g_image.Release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
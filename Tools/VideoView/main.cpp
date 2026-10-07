#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <chrono>
#include <string>
#include <mutex>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "Decoder.h"
#include "AudioRenderer.h"
#include "Gui.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

static HWND g_hwnd = nullptr;
static UINT g_width = 1280;
static UINT g_height = 720;
static bool g_running = true;

static AudioRenderer g_audio;

static std::string g_droppedFilePath;
static std::mutex  g_dropMutex;

// ---------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_device && wParam != SIZE_MINIMIZED)
        {
            g_width = LOWORD(lParam);
            g_height = HIWORD(lParam);

            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }

            if (g_swapChain)
            {
                g_swapChain->ResizeBuffers(0, g_width, g_height,
                    DXGI_FORMAT_UNKNOWN, 0);

                ID3D11Texture2D* backBuffer = nullptr;
                g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                    (void**)&backBuffer);
                if (backBuffer)
                {
                    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
                    backBuffer->Release();
                }
            }
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0)
        {
            wchar_t wpath[MAX_PATH] = {};
            if (DragQueryFileW(hDrop, 0, wpath, MAX_PATH) > 0)
            {
                int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                    nullptr, 0, nullptr, nullptr);
                if (len > 0)
                {
                    std::string utf8(len - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                        &utf8[0], len, nullptr, nullptr);
                    std::lock_guard<std::mutex> lk(g_dropMutex);
                    g_droppedFilePath = std::move(utf8);
                }
            }
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_DESTROY:
        DragAcceptFiles(hwnd, FALSE);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// D3D11 设备创建
// ---------------------------------------------------------------------------
static bool CreateD3D11Device()
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = g_width;
    sd.BufferDesc.Height = g_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL level;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, _countof(levels), D3D11_SDK_VERSION,
        &sd, &g_swapChain, &g_device, &level, &g_context);

    if (FAILED(hr) || !g_device || !g_context || !g_swapChain)
        return false;

    // 允许多线程访问 D3D11 设备
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(ID3D10Multithread),
        (void**)&mt)))
    {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
        (void**)&backBuffer)) && backBuffer)
    {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }

    return g_rtv != nullptr;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // 1) 注册窗口类
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"VideoViewerWindow";

    if (!RegisterClassEx(&wc)) return 1;

    // 2) 创建窗口
    g_hwnd = CreateWindowEx(
        0, wc.lpszClassName, L"Video Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        (int)g_width, (int)g_height,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd)
    {
        UnregisterClass(wc.lpszClassName, hInstance);
        return 1;
    }

    // 3) D3D11 设备
    if (!CreateD3D11Device())
    {
        MessageBox(nullptr, L"创建 D3D11 设备失败", L"错误", MB_OK | MB_ICONERROR);
        DestroyWindow(g_hwnd);
        UnregisterClass(wc.lpszClassName, hInstance);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // 4) 拖放支持
    DragAcceptFiles(g_hwnd, TRUE);
    // 若以管理员身份运行，UIPI 会拦截低完整性级别的拖放消息，需显式放行
    ChangeWindowMessageFilterEx(g_hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, 0x0049, MSGFLT_ALLOW, nullptr);

    // 5) 命令行参数（拖到 exe 上时 Windows 把路径作为参数传入）
    std::string initialFile;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            if (argc >= 2 && argv[1] && argv[1][0])
            {
                int len = WideCharToMultiByte(CP_UTF8, 0, argv[1], -1,
                    nullptr, 0, nullptr, nullptr);
                if (len > 0)
                {
                    initialFile.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, argv[1], -1,
                        &initialFile[0], len, nullptr, nullptr);
                }
            }
            LocalFree(argv);
        }
    }

    // 6) 初始化 GUI
    Gui gui;
    if (!gui.Init(g_hwnd, g_device, g_context))
    {
        MessageBox(nullptr, L"初始化 ImGui 失败", L"错误", MB_OK | MB_ICONERROR);
        if (g_rtv) { g_rtv->Release();       g_rtv = nullptr; }
        if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
        if (g_context) { g_context->Release();   g_context = nullptr; }
        if (g_device) { g_device->Release();    g_device = nullptr; }
        DestroyWindow(g_hwnd);
        UnregisterClass(wc.lpszClassName, hInstance);
        return 1;
    }

    // 7) 创建解码器 + 音频
    VideoDecoder decoder;
    decoder.SetAudioRenderer(&g_audio);
    gui.SetVolumeCallback([](float v) { g_audio.SetVolume(v); });

    // 打开命令行传入的文件
    if (!initialFile.empty())
    {
        if (decoder.Open(g_device, initialFile))
        {
            decoder.Play();
            gui.SetFilePath(initialFile);
        }
    }

    // 帧状态
    DecodedFrame currentFrame;
    DecodedFrame nextFrame;
    bool haveFrame = false;
    bool haveNextFrame = false;

    // FPS 统计
    auto  fpsTimer = std::chrono::steady_clock::now();
    int   frameCount = 0;
    float fpsDisplay = 0.0f;

    // ---- 主循环 ----
    while (g_running)
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        // 打开请求（UI 或拖放）
        std::string openPath;
        bool shouldOpen = false;

        if (gui.ConsumeOpenRequest(openPath))
            shouldOpen = !openPath.empty();

        if (!shouldOpen)
        {
            std::lock_guard<std::mutex> lk(g_dropMutex);
            if (!g_droppedFilePath.empty())
            {
                openPath = std::move(g_droppedFilePath);
                g_droppedFilePath.clear();
                shouldOpen = true;
            }
        }

        if (shouldOpen)
        {
            // 释放顺序：先帧，再解码器
            currentFrame.Reset();
            nextFrame.Reset();
            haveFrame = false;
            haveNextFrame = false;

            decoder.Close();
            if (decoder.Open(g_device, openPath))
            {
                decoder.Play();
                gui.SetFilePath(openPath);
            }
        }

        if (gui.ConsumeTogglePlayRequest())
        {
            if (decoder.IsOpen()) decoder.TogglePlay();
        }

        auto now = std::chrono::steady_clock::now();

        // 帧调度（音频主时钟）
        if (decoder.IsOpen())
        {
            if (!haveNextFrame)
            {
                DecodedFrame f;
                if (decoder.PopFrame(f, 0))
                {
                    nextFrame = std::move(f);
                    haveNextFrame = true;
                }
            }

            if (haveNextFrame)
            {
                double clock = decoder.MasterClock();
                const double tolerance = 0.005;
                if (nextFrame.ptsSec <= clock + tolerance)
                {
                    currentFrame = std::move(nextFrame);
                    haveFrame = true;
                    haveNextFrame = false;
                }
            }
        }

        // 转换为 SRV
        ID3D11ShaderResourceView* srv = nullptr;
        int videoW = 0, videoH = 0;

        if (haveFrame && currentFrame.frame)
        {
            AVFrame* f = currentFrame.frame;

            if (currentFrame.isHardware && f->format == AV_PIX_FMT_D3D11)
            {
                ID3D11Texture2D* tex = (ID3D11Texture2D*)f->data[0];
                UINT arrayIndex = (UINT)(intptr_t)f->data[1];
                videoW = decoder.Width();
                videoH = decoder.Height();
                srv = gui.ConvertFrameToTexture(tex, arrayIndex, videoW, videoH);
            }
            else if (f->format == AV_PIX_FMT_BGRA)
            {
                videoW = f->width;
                videoH = f->height;
                srv = gui.UploadBgraFrame(f->data[0], f->linesize[0],
                    f->width, f->height);
            }
        }
        gui.SetVideoTexture(srv, videoW, videoH);

        // FPS 统计
        ++frameCount;
        double elapsed = std::chrono::duration<double>(now - fpsTimer).count();
        if (elapsed >= 0.5)
        {
            fpsDisplay = (float)(frameCount / elapsed);
            frameCount = 0;
            fpsTimer = now;
        }

        // ImGui
        gui.BeginFrame();
        gui.DrawUI(decoder, fpsDisplay);
        gui.EndFrame();

        // 渲染
        const float clearColor[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clearColor);

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swapChain->Present(1, 0);
    }

    // ---- 清理（顺序：先帧，再解码器，最后 D3D11） ----
    currentFrame.Reset();
    nextFrame.Reset();
    decoder.Close();
    gui.Shutdown();

    if (g_rtv) { g_rtv->Release();       g_rtv = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release();   g_context = nullptr; }
    if (g_device) { g_device->Release();    g_device = nullptr; }

    DestroyWindow(g_hwnd);
    UnregisterClass(wc.lpszClassName, hInstance);

    return 0;
}
#include "..\core\TextGameAPI.h"

#include <windows.h>
#include <d3d11.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "TextureManager.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>

// ImGui Win32 消息处理前向声明

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);


// 共享 UI 状态

struct LogLine {
    std::string name;
    ImU32       color;
    std::string text;
};

struct UIState {
    std::mutex               mtx;
    std::condition_variable  cv;

    std::deque<LogLine> log;

    std::string backgroundPath;
    std::string leftSpritePath;
    std::string rightSpritePath;
    std::string avatarPath;

    // 当前说话者所在侧
    std::string speakingSide;

    // 挂起中的菜单（只有游戏线程在写，GUI 线程读写都在锁内）
    bool                     menuActive = false;
    std::string              menuTitle;
    std::vector<std::string> menuChoices;
    int                      menuResult = -1;   // >=0 表示 GUI 已选定

    std::atomic<bool>        gameFinished{ false };
    std::atomic<bool>        windowClosed{ false };
};

static UIState        g_ui;
static TextureManager g_tex;


// DX11 全局

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static UINT                    g_ResizeWidth = 0;
static UINT                    g_ResizeHeight = 0;

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
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
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (FAILED(res)) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// 工具

static ImU32 ParseColor(const std::string& hex) {
    if (hex.size() >= 7 && hex[0] == '#') {
        unsigned r = 0, g = 0, b = 0;
        if (sscanf_s(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3)
            return IM_COL32(r, g, b, 255);
    }
    return IM_COL32_WHITE;
}

static ImTextureID ToTexID(ID3D11ShaderResourceView* srv) {
    return (ImTextureID)(intptr_t)srv;
}

// 游戏线程

static void GameThreadProc(textgame::TextGameCore* core, std::string scriptPath) {
    auto r = core->RunScriptFile(scriptPath);
    if (!r.ok) {
        std::lock_guard<std::mutex> lk(g_ui.mtx);
        g_ui.log.push_back({ "", IM_COL32(255,80,80,255), "[错误] " + r.error });
    }
    else {
        core->LaunchGame("start");
    }
    g_ui.gameFinished = true;
    g_ui.cv.notify_all();
}


// 入口

int WINAPI wWinMain(
    _In_     HINSTANCE /*hInstance*/,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_     PWSTR     /*pCmdLine*/,
    _In_     int       /*nCmdShow*/)
{
    // 以 exe 目录为 CWD
    {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    // 路径为 TextGame/runtime/runtime.exe 所以需要向上一级
    namespace fs = std::filesystem;
    fs::path dir = fs::path(exePath).parent_path().parent_path();
#ifdef _DEBUG
    // debug模式下路径为 TextGame/runtime/x64/runtime.exe 所以需要再向上一级
    dir = dir.parent_path();
#endif // _DEBUG
    fs::current_path(dir);
    }


    // 1. 创建窗口
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"TextGameWindow", nullptr
    };
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(
        wc.lpszClassName, L"TextGame",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1280, 720,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);


    // 2. ImGui 初始化

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;

    // 加载微软雅黑字体
    io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\msyh.ttc", 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // --------------------------------------------------------------
    // 3. 配置 + 启动引擎
    // --------------------------------------------------------------
    textgame::GameConfig cfg;
    cfg.assetRoot = "core";
    cfg.levelRoot = "Levels/";
    cfg.pythonHome = "Libraries/python/";
    cfg.useSandbox = true;
    cfg.sandboxMB = 64;
    cfg.devMode = true;

    // --- 回调：日志类，直接 push 到 log ---
    cfg.onSpeak = [](const std::string& t) {
        std::lock_guard<std::mutex> lk(g_ui.mtx);
        g_ui.log.push_back({ "", IM_COL32_WHITE, t });
    };

    cfg.onCharacterSpeak = [](const std::string& name,
        const std::string& color,
        const std::string& text,
        const std::string& sprite,
        const std::string& avatar,
        const std::string& side) {
            std::lock_guard<std::mutex> lk(g_ui.mtx);
            g_ui.log.push_back({ name, ParseColor(color), text });

            // 立绘：非空才更新（旁白不会误清空其它角色立绘）
            if (!sprite.empty()) {
                if (side == "right") g_ui.rightSpritePath = sprite;
                else                 g_ui.leftSpritePath = sprite;
            }
            // 头像：始终取最新（空 = 清空，用于旁白）
            g_ui.avatarPath = avatar;

            // 说话者所在侧：旁白清空，角色按 side 判定
            if (name.empty()) {
                g_ui.speakingSide.clear();
            }
            else {
                g_ui.speakingSide = (side == "right") ? "right" : "left";
            }
        };

    cfg.onTextPrint = [](const std::vector<std::string>& lines) {
        std::lock_guard<std::mutex> lk(g_ui.mtx);
        for (auto& s : lines) g_ui.log.push_back({ "", IM_COL32_WHITE, s });
    };

    cfg.onLoad = [](const std::string& p) {
        std::lock_guard<std::mutex> lk(g_ui.mtx);
        g_ui.log.push_back({ "", IM_COL32(180,180,180,255), "[load] " + p });
    };

    cfg.onLoadScene = [](const std::string& p) {
        std::lock_guard<std::mutex> lk(g_ui.mtx);
        g_ui.backgroundPath = p;
        // 切场景清场
        g_ui.leftSpritePath.clear();
        g_ui.rightSpritePath.clear();
        g_ui.avatarPath.clear();
        g_ui.speakingSide.clear();
    };

    cfg.onClearCharacters = [] {
        std::lock_guard<std::mutex> lk(g_ui.mtx);
        g_ui.leftSpritePath.clear();
        g_ui.rightSpritePath.clear();
        g_ui.avatarPath.clear();
        g_ui.speakingSide.clear();
        };

    // --- 回调：菜单，阻塞游戏线程直到 GUI 给出结果 ---
    cfg.onMenu = [](const std::string& title,
        const std::vector<std::string>& choices) -> int {
            std::unique_lock<std::mutex> lk(g_ui.mtx);
            g_ui.menuTitle = title;
            g_ui.menuChoices = choices;
            g_ui.menuActive = true;
            g_ui.menuResult = -1;

            {
                textgame::GILRelease release;   // 等待期间让出 GIL
                g_ui.cv.wait(lk, [] {
                    return g_ui.menuResult >= 0 || g_ui.windowClosed.load();
                });
            }

            if (g_ui.windowClosed.load()) {
                // 通知 Python 层立即终止脚本
                textgame::RaiseScriptTermination("window closed");
                g_ui.menuActive = false;
                g_ui.menuChoices.clear();
                g_ui.menuResult = -1;
                return 0;
            }

            int r = g_ui.menuResult;
            g_ui.menuActive = false;
            g_ui.menuChoices.clear();
            g_ui.menuResult = -1;
            return (r < 0) ? 0 : r;
    };

    // 启动引擎 + 游戏线程
    auto core = std::make_unique<textgame::TextGameCore>();
    std::thread gameThread;

    if (!core->Init(cfg)) {
        std::lock_guard<std::mutex> lk(g_ui.mtx);
        g_ui.log.push_back({ "", IM_COL32(255,80,80,255), u8"[错误] 引擎初始化失败" });
        g_ui.gameFinished = true;
    }
    else {
        g_tex.Init(g_pd3dDevice);
        // 自动加载关卡脚本，读取 Levels/main.tgl 并运行。
        gameThread = std::thread(GameThreadProc, core.get(), std::string(cfg.levelRoot + "main.tgl"));
    }

    // 4. 主循环

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // 最小化时略过渲染
        if (::IsIconic(hwnd)) {
            ::Sleep(10);
            continue;
        }

        // 处理窗口缩放
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ---- 全屏主窗口 ----
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##GameWindow", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar();

        ImVec2 areaPos = ImGui::GetCursorScreenPos();
        ImVec2 areaSize = ImGui::GetContentRegionAvail();

        // ---- 取状态 ----
        std::string bgPath, leftPath, rightPath, avPath;
        std::string speakingSide;
        bool menuActive = false;
        std::string menuTitle;
        std::vector<std::string> menuChoices;
        {
            std::lock_guard<std::mutex> lk(g_ui.mtx);
            bgPath = g_ui.backgroundPath;
            leftPath = g_ui.leftSpritePath;
            rightPath = g_ui.rightSpritePath;
            avPath = g_ui.avatarPath;
            speakingSide = g_ui.speakingSide;
            menuActive = g_ui.menuActive;
            menuTitle = g_ui.menuTitle;
            menuChoices = g_ui.menuChoices;
        }
        bool gameFinished = g_ui.gameFinished.load();

        // ---- 按需加载纹理 ----
        auto* bgTex = g_tex.GetOrLoad(bgPath);
        auto* leftTex = g_tex.GetOrLoad(leftPath);
        auto* rightTex = g_tex.GetOrLoad(rightPath);
        auto* avTex = g_tex.GetOrLoad(avPath);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ---- 1. 背景（铺满） ----
        if (bgTex) {
            dl->AddImage(ToTexID(bgTex), areaPos,
                ImVec2(areaPos.x + areaSize.x, areaPos.y + areaSize.y));
        }
        else {
            dl->AddRectFilled(areaPos,
                ImVec2(areaPos.x + areaSize.x, areaPos.y + areaSize.y),
                IM_COL32(18, 18, 28, 255));
        }

        // ---- 2. 左右立绘（128 x 256 原尺寸，不缩放） ----
        const float SPRITE_W = 128.0f;
        const float SPRITE_H = 256.0f;
        const float SPRITE_Y = areaPos.y + areaSize.y * 0.10f;
        const float SPRITE_MARGIN = 40.0f;   // 距窗口侧边的留白

        // 亮度参数
        const ImU32 TINT_NORMAL = IM_COL32(255, 255, 255, 255);
        const ImU32 TINT_DIMMED = IM_COL32(170, 170, 170, 255);

        // 旁白（speakingSide 为空）两侧压暗
        auto sideTint = [&](const char* side) -> ImU32 {
            return (speakingSide == side) ? TINT_NORMAL : TINT_DIMMED;
        };

        if (leftTex) {
            float x = areaPos.x + SPRITE_MARGIN;
            dl->AddImage(ToTexID(leftTex),
                ImVec2(x, SPRITE_Y),
                ImVec2(x + SPRITE_W, SPRITE_Y + SPRITE_H),
                ImVec2(0, 0), ImVec2(1, 1),
                sideTint("left"));
        }

        if (rightTex) {
            float x = areaPos.x + areaSize.x - SPRITE_MARGIN - SPRITE_W;
            dl->AddImage(ToTexID(rightTex),
                ImVec2(x, SPRITE_Y),
                ImVec2(x + SPRITE_W, SPRITE_Y + SPRITE_H),
                ImVec2(0, 0), ImVec2(1, 1),
                sideTint("right"));
        }

        // ---- 3. 对话框 ----
        const float DIALOG_H = 200.0f;
        const float DIALOG_MARGIN = 20.0f;
        ImVec2 dialogMin(areaPos.x + DIALOG_MARGIN,
            areaPos.y + areaSize.y - DIALOG_H - DIALOG_MARGIN);
        ImVec2 dialogMax(areaPos.x + areaSize.x - DIALOG_MARGIN,
            areaPos.y + areaSize.y - DIALOG_MARGIN);

        dl->AddRectFilled(dialogMin, dialogMax, IM_COL32(15, 15, 25, 235), 10.0f);
        dl->AddRect(dialogMin, dialogMax, IM_COL32(80, 80, 130, 255), 10.0f, 0, 2.0f);

        // ---- 3a. 头像 128x128 ----
        const float AVATAR_SIZE = 128.0f;
        ImVec2 avMin(dialogMin.x + 16,
            dialogMin.y + (DIALOG_H - AVATAR_SIZE) * 0.5f);
        ImVec2 avMax(avMin.x + AVATAR_SIZE, avMin.y + AVATAR_SIZE);

        if (avTex) {
            dl->AddImage(ToTexID(avTex), avMin, avMax);
        }
        else {
            dl->AddRectFilled(avMin, avMax, IM_COL32(40, 40, 60, 255));
        }
        dl->AddRect(avMin, avMax, IM_COL32(100, 100, 160, 255), 0, 0, 2.0f);

        // ---- 3b. 对话文本（滚动区域） ----
        float textLeft = avMax.x + 16;
        float textRight = dialogMax.x - 16;
        float textTop = dialogMin.y + 12;
        float textBottom = dialogMax.y - 12;

        ImGui::SetCursorScreenPos(ImVec2(textLeft, textTop));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##dialogue_text",
            ImVec2(textRight - textLeft, textBottom - textTop),
            false, ImGuiWindowFlags_NoBackground);

        {
            std::lock_guard<std::mutex> lk(g_ui.mtx);
            const bool wasAtBottom =
                ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

            for (const auto& line : g_ui.log) {
                if (!line.name.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, line.color);
                    ImGui::TextUnformatted((line.name + u8"：").c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 0);
                }
                ImGui::TextWrapped("%s", line.text.c_str());
            }
            if (wasAtBottom) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // ---- 4. 菜单（对话框上方浮动） ----
        if (menuActive) {
            float menuH = 56.0f + menuChoices.size() * 40.0f;
            ImVec2 mMin(areaPos.x + areaSize.x * 0.25f, dialogMin.y - menuH - 20);
            ImVec2 mMax(areaPos.x + areaSize.x * 0.75f, dialogMin.y - 20);

            dl->AddRectFilled(mMin, mMax, IM_COL32(20, 20, 35, 240), 10.0f);
            dl->AddRect(mMin, mMax, IM_COL32(100, 100, 160, 255), 10.0f, 0, 2.0f);

            ImGui::SetCursorScreenPos(ImVec2(mMin.x + 16, mMin.y + 12));
            ImGui::BeginGroup();
            ImGui::TextUnformatted(menuTitle.c_str());
            ImGui::Spacing();

            for (size_t i = 0; i < menuChoices.size(); ++i) {
                std::string label = menuChoices[i] + "##menu_" + std::to_string(i);
                if (ImGui::Button(label.c_str(),
                    ImVec2(mMax.x - mMin.x - 32, 32))) {
                    std::lock_guard<std::mutex> lk(g_ui.mtx);
                    g_ui.menuResult = (int)i;
                    g_ui.cv.notify_all();
                }
            }
            ImGui::EndGroup();
        }

        // ---- 5. 结束提示 ----
        if (gameFinished && !menuActive) {
            const char* t = u8"（游戏已结束，关闭窗口退出）";
            ImVec2 sz = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(areaPos.x + areaSize.x - sz.x - 16,
                areaPos.y + 16),
                IM_COL32(180, 180, 200, 220), t);
        }
        ImGui::End();

        // ---- 渲染 ----
        ImGui::Render();
        const float clear_color[4] = { 0.05f, 0.05f, 0.08f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // 垂直同步
    }


    // 5. 收尾：先唤醒/汇合游戏线程，再关引擎和图形

    g_ui.windowClosed = true;
    g_ui.cv.notify_all();
    if (gameThread.joinable()) gameThread.join();

    if (core) { core->Shutdown(); core.reset(); }
    g_tex.Shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
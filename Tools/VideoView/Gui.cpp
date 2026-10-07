#include "Gui.h"
#include "Decoder.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <cstring>
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

Gui::Gui() {}
Gui::~Gui() { Shutdown(); }

bool Gui::Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context)
{
    hwnd_ = hwnd;
    device_ = device;
    context_ = context;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd)) return false;
    if (!ImGui_ImplDX11_Init(device, context)) return false;

    if (FAILED(device_->QueryInterface(__uuidof(ID3D11VideoDevice),
        (void**)&videoDevice_)))
        return false;
    if (FAILED(context_->QueryInterface(__uuidof(ID3D11VideoContext),
        (void**)&videoContext_)))
        return false;

    filePath_[0] = '\0';
    return true;
}

void Gui::Shutdown()
{
    ReleaseRenderResources();
    ReleaseSoftRenderResources();
    ReleaseVideoProcessor();

    if (videoContext_) { videoContext_->Release(); videoContext_ = nullptr; }
    if (videoDevice_) { videoDevice_->Release();  videoDevice_ = nullptr; }

    if (ImGui::GetCurrentContext())
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    device_ = nullptr;
    context_ = nullptr;
    hwnd_ = nullptr;
}

// ---------------------------------------------------------------------------
bool Gui::OpenFileDialog(std::string& outPath)
{
    wchar_t filename[MAX_PATH] = {};
    // 若文本框里已有路径，把它作为对话框初始选择
    if (filePath_[0] != '\0')
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, filePath_, -1,
            filename, MAX_PATH);
        if (n <= 0) filename[0] = L'\0';
    }

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter =
        L"媒体文件\0*.mp4;*.mkv;*.avi;*.mov;*.flv;*.wmv;*.webm;*.ts;*.m4v;"
        L"*.mp3;*.aac;*.wav;*.flac;*.ogg;*.m4a\0"
        L"视频文件\0*.mp4;*.mkv;*.avi;*.mov;*.flv;*.wmv;*.webm;*.ts\0"
        L"音频文件\0*.mp3;*.aac;*.wav;*.flac;*.ogg;*.m4a\0"
        L"所有文件\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"打开媒体文件";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn))
        return false;   // 用户取消

    int len = WideCharToMultiByte(CP_UTF8, 0, filename, -1,
        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;

    outPath.resize(len - 1);
    WideCharToMultiByte(CP_UTF8, 0, filename, -1,
        &outPath[0], len, nullptr, nullptr);
    return true;
}

bool Gui::InitVideoProcessor()
{
    ReleaseVideoProcessor();

    if (!videoDevice_ || renderW_ <= 0 || renderH_ <= 0)
        return false;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = renderW_;
    cd.InputHeight = renderH_;
    cd.OutputWidth = renderW_;
    cd.OutputHeight = renderH_;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&cd, &vpEnum_);
    if (FAILED(hr) || !vpEnum_) return false;

    hr = videoDevice_->CreateVideoProcessor(vpEnum_, 0, &vp_);
    if (FAILED(hr) || !vp_) return false;

    if (renderTex_ && !outputView_)
    {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice = 0;

        hr = videoDevice_->CreateVideoProcessorOutputView(
            renderTex_, vpEnum_, &ovd, &outputView_);
        if (FAILED(hr) || !outputView_) return false;
    }

    return true;
}

void Gui::ReleaseVideoProcessor()
{
    if (outputView_) { outputView_->Release(); outputView_ = nullptr; }
    if (vp_) { vp_->Release();         vp_ = nullptr; }
    if (vpEnum_) { vpEnum_->Release();     vpEnum_ = nullptr; }
}

// ---------------------------------------------------------------------------
void Gui::ReleaseRenderResources()
{
    if (outputView_) { outputView_->Release(); outputView_ = nullptr; }
    if (renderSRV_) { renderSRV_->Release();  renderSRV_ = nullptr; }
    if (renderTex_) { renderTex_->Release();  renderTex_ = nullptr; }
    renderW_ = renderH_ = 0;
}

void Gui::ReleaseSoftRenderResources()
{
    if (swSRV_) { swSRV_->Release(); swSRV_ = nullptr; }
    if (swTex_) { swTex_->Release(); swTex_ = nullptr; }
    swW_ = swH_ = 0;
}

void Gui::EnsureRenderTexture(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (renderTex_ && renderW_ == w && renderH_ == h && outputView_)
        return;

    ReleaseRenderResources();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device_->CreateTexture2D(&td, nullptr, &renderTex_)) || !renderTex_)
        return;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;

    if (FAILED(device_->CreateShaderResourceView(renderTex_, &sd, &renderSRV_)) || !renderSRV_)
        return;

    renderW_ = w;
    renderH_ = h;

    InitVideoProcessor();
}

// ---------------------------------------------------------------------------
ID3D11ShaderResourceView* Gui::ConvertFrameToTexture(ID3D11Texture2D* tex,
    UINT arrayIndex,
    int width, int height)
{
    if (!tex || !videoDevice_ || !videoContext_) return nullptr;

    EnsureRenderTexture(width, height);
    if (!renderTex_ || !vp_ || !outputView_) return nullptr;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = arrayIndex;

    ID3D11VideoProcessorInputView* inputView = nullptr;
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
        tex, vpEnum_, &ivd, &inputView);
    if (FAILED(hr) || !inputView) return nullptr;

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView;

    hr = videoContext_->VideoProcessorBlt(vp_, outputView_, 0, 1, &stream);

    inputView->Release();

    if (FAILED(hr)) return nullptr;

    return renderSRV_;
}

// ---------------------------------------------------------------------------
ID3D11ShaderResourceView* Gui::UploadBgraFrame(const uint8_t* data,
    int lineSize,
    int width, int height)
{
    if (!data || width <= 0 || height <= 0) return nullptr;

    if (!swTex_ || swW_ != width || swH_ != height)
    {
        ReleaseSoftRenderResources();

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)width;
        td.Height = (UINT)height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(device_->CreateTexture2D(&td, nullptr, &swTex_)) || !swTex_)
            return nullptr;

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;

        if (FAILED(device_->CreateShaderResourceView(swTex_, &sd, &swSRV_)) || !swSRV_)
            return nullptr;

        swW_ = width;
        swH_ = height;
    }

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(context_->Map(swTex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        for (int y = 0; y < height; ++y)
        {
            std::memcpy((uint8_t*)ms.pData + y * ms.RowPitch,
                data + y * lineSize,
                (size_t)width * 4);
        }
        context_->Unmap(swTex_, 0);
    }

    return swSRV_;
}

// ---------------------------------------------------------------------------
void Gui::BeginFrame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Gui::EndFrame()
{
    ImGui::Render();
}

// ---------------------------------------------------------------------------
void Gui::SetFilePath(const std::string& path)
{
    strncpy_s(filePath_, path.c_str(), _TRUNCATE);
}

// ---------------------------------------------------------------------------
void Gui::DrawUI(VideoDecoder& decoder, float fpsDisplay)
{
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    // ------- 工具栏 -------
    ImGui::BeginChild("##toolbar", ImVec2(0, 46), true);
    ImGui::SetCursorPosY(10);

    if (ImGui::Button(decoder.IsPlaying() ? "Pause" : "Play",
        ImVec2(72, 26)))
    {
        togglePlayRequested_ = true;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("##file", filePath_, IM_ARRAYSIZE(filePath_));

    ImGui::SameLine();
    if (ImGui::Button("Open", ImVec2(72, 26)))
    {
        std::string picked;
        if (OpenFileDialog(picked))
        {
            // 同步文本框显示
            strncpy_s(filePath_, picked.c_str(), _TRUNCATE);
            // 交给主循环去 Close/Open
            openRequested_ = true;
            openPath_ = std::move(picked);
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("##vol", &volume_, 0.0f, 2.0f, "Vol %.2f"))
    {
        if (volumeChangedCallback_) volumeChangedCallback_(volume_);
    }

    ImGui::SameLine();
    if (decoder.IsOpen())
    {
        ImGui::Text("| %dx%d | %.1f FPS | %.2f / %.2f s | %s",
            decoder.Width(), decoder.Height(), fpsDisplay,
            decoder.Position(), decoder.Duration(),
            decoder.HasAudio() ? "A+V" : "V only");
    }
    else
    {
        ImGui::Text("| %.1f FPS (idle)", fpsDisplay);
    }

    if (!decoder.LastError().empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
            "| %s", decoder.LastError().c_str());
    }

    ImGui::EndChild();

    // ------- 视频区域 -------
    ImVec2 regionSize = ImGui::GetContentRegionAvail();
    if (regionSize.x < 1.0f) regionSize.x = 1.0f;
    if (regionSize.y < 1.0f) regionSize.y = 1.0f;

    ImGui::BeginChild("##video", regionSize, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (currentSRV_ && videoWidth_ > 0 && videoHeight_ > 0)
    {
        float regionW = regionSize.x;
        float regionH = regionSize.y;
        float videoAspect = (float)videoWidth_ / (float)videoHeight_;
        float regionAspect = regionW / regionH;

        float drawW, drawH;
        if (videoAspect > regionAspect)
        {
            drawW = regionW;
            drawH = regionW / videoAspect;
        }
        else
        {
            drawH = regionH;
            drawW = regionH * videoAspect;
        }

        float offsetX = (regionW - drawW) * 0.5f;
        float offsetY = (regionH - drawH) * 0.5f;

        ImGui::SetCursorPos(ImVec2(offsetX, offsetY));
        ImGui::Image((ImTextureID)(intptr_t)currentSRV_, ImVec2(drawW, drawH));
    }
    else
    {
        const char* text = decoder.IsOpen()
            ? "Waiting for frames..."
            : "Open a video file or drag it here";
        ImVec2 textSize = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2(
            (regionSize.x - textSize.x) * 0.5f,
            (regionSize.y - textSize.y) * 0.5f));
        ImGui::TextUnformatted(text);
    }

    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
bool Gui::ConsumeOpenRequest(std::string& outPath)
{
    if (!openRequested_) return false;
    openRequested_ = false;
    outPath = openPath_;
    openPath_.clear();
    return true;
}

bool Gui::ConsumeTogglePlayRequest()
{
    if (!togglePlayRequested_) return false;
    togglePlayRequested_ = false;
    return true;
}
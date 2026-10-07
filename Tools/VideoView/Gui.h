#pragma once

#include <d3d11.h>
#include <string>
#include <functional>

class VideoDecoder;

class Gui
{
public:
    Gui();
    ~Gui();

    bool Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    void BeginFrame();
    void EndFrame();

    // D3D11 硬解帧 → BGRA 渲染纹理（Video Processor 硬件转换）
    ID3D11ShaderResourceView* ConvertFrameToTexture(ID3D11Texture2D* tex,
        UINT arrayIndex,
        int width, int height);

    // 软解 BGRA 帧 → 动态纹理上传
    ID3D11ShaderResourceView* UploadBgraFrame(const uint8_t* data,
        int lineSize,
        int width, int height);

    void SetVideoTexture(ID3D11ShaderResourceView* srv, int w, int h)
    {
        currentSRV_ = srv;
        videoWidth_ = w;
        videoHeight_ = h;
    }

    void DrawUI(VideoDecoder& decoder, float fpsDisplay);

    void SetFilePath(const std::string& path);
    void SetVolumeCallback(std::function<void(float)> cb) { volumeChangedCallback_ = std::move(cb); }

    bool ConsumeOpenRequest(std::string& outPath);
    bool ConsumeTogglePlayRequest();

private:
    bool InitVideoProcessor();
    void ReleaseVideoProcessor();
    void EnsureRenderTexture(int w, int h);
    void ReleaseRenderResources();
    void ReleaseSoftRenderResources();

    bool OpenFileDialog(std::string& outPath);

    HWND                 hwnd_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    // Video Processor（硬解路径）
    ID3D11VideoDevice* videoDevice_ = nullptr;
    ID3D11VideoContext* videoContext_ = nullptr;
    ID3D11VideoProcessorEnumerator* vpEnum_ = nullptr;
    ID3D11VideoProcessor* vp_ = nullptr;

    ID3D11Texture2D* renderTex_ = nullptr;
    ID3D11ShaderResourceView* renderSRV_ = nullptr;
    ID3D11VideoProcessorOutputView* outputView_ = nullptr;
    int                             renderW_ = 0;
    int                             renderH_ = 0;

    // 软解路径
    ID3D11Texture2D* swTex_ = nullptr;
    ID3D11ShaderResourceView* swSRV_ = nullptr;
    int                       swW_ = 0;
    int                       swH_ = 0;

    ID3D11ShaderResourceView* currentSRV_ = nullptr;
    int                       videoWidth_ = 0;
    int                       videoHeight_ = 0;

    char        filePath_[512] = "";
    bool        openRequested_ = false;
    std::string openPath_;
    bool        togglePlayRequested_ = false;
    float       volume_ = 1.0f;

    std::function<void(float)> volumeChangedCallback_;
};
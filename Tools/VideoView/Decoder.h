#pragma once

#include <d3d11.h>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

class AudioRenderer;

// ---------------------------------------------------------------------------
// DecodedFrame：持有解码后的 AVFrame
// 硬解时 data[0] 是 ID3D11Texture2D*，data[1] 是纹理数组索引
// 软解时 data[0] 是 BGRA 像素数据
// ---------------------------------------------------------------------------
struct DecodedFrame
{
    AVFrame* frame = nullptr;
    int64_t  pts = 0;
    double   ptsSec = 0.0;
    double   durationSec = 0.0;
    bool     isHardware = false;

    DecodedFrame() = default;
    explicit DecodedFrame(AVFrame* f);
    ~DecodedFrame();

    DecodedFrame(DecodedFrame&& other) noexcept;
    DecodedFrame& operator=(DecodedFrame&& other) noexcept;

    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;

    void Reset();
    bool Valid() const { return frame != nullptr; }
};

// ---------------------------------------------------------------------------
// VideoDecoder：视频 + 音频解码后端
// 三线程架构：Demux / VideoDecode / AudioDecode
// 音频时钟作为主时钟，视频按此时钟调度
// ---------------------------------------------------------------------------
class VideoDecoder
{
public:
    VideoDecoder();
    ~VideoDecoder();

    // 注入音频渲染器（必须在 Open 之前调用）
    void SetAudioRenderer(AudioRenderer* ar) { audioRenderer_ = ar; }

    bool Open(ID3D11Device* device, const std::string& filePath);
    void Close();

    void Play();
    void Pause();
    void TogglePlay();
    bool IsPlaying() const { return playing_.load(std::memory_order_relaxed); }

    bool PopFrame(DecodedFrame& out, int timeoutMs = 0);
    void ClearFrames();

    int    Width()    const { return width_; }
    int    Height()   const { return height_; }
    double Duration() const { return duration_; }
    double Fps()      const { return fps_; }
    bool   HasVideo() const { return videoStreamIndex_ >= 0; }
    bool   HasAudio() const { return audioStreamIndex_ >= 0; }
    bool   IsOpen()   const { return fmtCtx_ != nullptr; }
    const std::string& LastError() const { return error_; }

    // 主时钟（秒）：优先音频时钟，无音频回退系统时钟
    double MasterClock() const;

    // 当前播放位置（秒）—— 语义等价于主时钟
    double Position() const { return MasterClock(); }

private:
    void DemuxLoop();
    void VideoDecodeLoop();
    void AudioDecodeLoop();
    void PushFrameToQueue(AVFrame* f, double ptsSec, double durSec, bool isHw);
    AVPacket* PopVideoPacket(int timeoutMs, bool& eof);
    AVPacket* PopAudioPacket(int timeoutMs, bool& eof);

    ID3D11Device* device_ = nullptr;
    AudioRenderer* audioRenderer_ = nullptr;

    AVFormatContext* fmtCtx_ = nullptr;
    AVCodecContext* videoCodecCtx_ = nullptr;
    AVCodecContext* audioCodecCtx_ = nullptr;
    AVBufferRef* hwDeviceCtx_ = nullptr;
    SwrContext* swrCtx_ = nullptr;
    SwsContext* swsCtx_ = nullptr;   // 软解回退 YUV→BGRA
    int              swsSrcFmt_ = -1;
    int              swsSrcW_ = 0;
    int              swsSrcH_ = 0;

    int    videoStreamIndex_ = -1;
    int    audioStreamIndex_ = -1;

    int    width_ = 0;
    int    height_ = 0;
    double duration_ = 0.0;
    double fps_ = 0.0;
    std::string error_;

    // 包队列（Demux → 解码线程）
    std::mutex              packetMutex_;
    std::condition_variable packetCv_;
    std::queue<AVPacket*>   videoPackets_;
    std::queue<AVPacket*>   audioPackets_;
    bool                    demuxEof_ = false;

    // 帧队列（VideoDecode → 渲染线程）
    mutable std::mutex       queueMutex_;
    std::condition_variable  queueCv_;
    std::queue<DecodedFrame> frameQueue_;
    size_t                   maxQueueSize_ = 4;

    std::thread       demuxThread_;
    std::thread       videoThread_;
    std::thread       audioThread_;
    std::atomic<bool> abort_{ false };
    std::atomic<bool> playing_{ false };

    // 时钟基准
    std::atomic<double> firstAudioPts_{ -1.0 };
    std::atomic<double> firstVideoPts_{ -1.0 };
    std::chrono::steady_clock::time_point playbackStart_;
    std::atomic<bool>   playbackStartSet_{ false };
};
#pragma once

#include <windows.h>
#include <xaudio2.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>

// 基于 XAudio2 的音频输出，含环形缓冲区 + 喂送线程
// 通过 IXAudio2VoiceCallback 的 OnBufferEnd 推进播放样本计数，构成音频时钟
class AudioRenderer
{
public:
    AudioRenderer();
    ~AudioRenderer();

    bool Init(int sampleRate, int channels);
    void Shutdown();

    void Play();
    void Pause();
    void Stop();

    bool Write(const uint8_t* data, size_t bytes, const std::atomic<bool>& abort);

    uint64_t PlayedSamples() const { return playedSamples_.load(std::memory_order_relaxed); }
    double   PlayedSeconds() const
    {
        return sampleRate_ > 0 ? (double)PlayedSamples() / (double)sampleRate_ : 0.0;
    }

    size_t BufferedBytes() const
    {
        return (size_t)(writePos_.load(std::memory_order_relaxed)
            - readPos_.load(std::memory_order_relaxed));
    }

    int  SampleRate() const { return sampleRate_; }
    int  Channels()   const { return channels_; }
    bool IsReady()    const { return source_ != nullptr; }

    void SetVolume(float v);

private:
    class VoiceCB;
    friend class VoiceCB;

    void FeedThread();

    static const int    kNumSubmitBufs = 8;
    static const size_t kChunkMs = 20;

    struct SubmitBuf
    {
        std::vector<uint8_t> data;
        std::atomic<bool>    inUse{ false };
        uint32_t             samples = 0;
    };

    IXAudio2* xaudio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* source_ = nullptr;
    VoiceCB* voiceCB_ = nullptr;

    int sampleRate_ = 48000;
    int channels_ = 2;

    std::vector<uint8_t>  ring_;
    size_t                ringCapacity_ = 0;
    std::atomic<uint64_t> writePos_{ 0 };
    std::atomic<uint64_t> readPos_{ 0 };
    std::mutex            ringMutex_;

    SubmitBuf submitBufs_[kNumSubmitBufs];

    std::atomic<uint64_t> playedSamples_{ 0 };
    std::thread           feedThread_;
    std::atomic<bool>     feeding_{ false };
    std::atomic<bool>     paused_{ false };
};
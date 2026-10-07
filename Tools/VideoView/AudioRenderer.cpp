#include "AudioRenderer.h"
#include <chrono>
#include <algorithm>

#pragma comment(lib, "xaudio2.lib")

// ---------------------------------------------------------------------------
// XAudio2 回调：每个提交块播完时触发
// ---------------------------------------------------------------------------
class AudioRenderer::VoiceCB : public IXAudio2VoiceCallback
{
public:
    AudioRenderer* owner = nullptr;

    void STDMETHODCALLTYPE OnBufferEnd(void* pBufferContext) override
    {
        if (!owner || !pBufferContext) return;
        auto* sb = (SubmitBuf*)pBufferContext;
        owner->playedSamples_.fetch_add(sb->samples, std::memory_order_relaxed);
        sb->inUse.store(false, std::memory_order_release);
    }

    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

// ---------------------------------------------------------------------------
AudioRenderer::AudioRenderer() {}
AudioRenderer::~AudioRenderer() { Shutdown(); }

// ---------------------------------------------------------------------------
bool AudioRenderer::Init(int sampleRate, int channels)
{
    Shutdown();

    sampleRate_ = sampleRate;
    channels_ = channels;

    HRESULT hr = XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !xaudio_) return false;

    hr = xaudio_->CreateMasteringVoice(&master_);
    if (FAILED(hr) || !master_)
    {
        xaudio_->Release(); xaudio_ = nullptr;
        return false;
    }

    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels_;
    wf.nSamplesPerSec = (DWORD)sampleRate_;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(channels_ * 2);
    wf.nAvgBytesPerSec = (DWORD)(sampleRate_ * wf.nBlockAlign);
    wf.cbSize = 0;

    voiceCB_ = new VoiceCB();
    voiceCB_->owner = this;

    hr = xaudio_->CreateSourceVoice(&source_, &wf, 0,
        XAUDIO2_DEFAULT_FREQ_RATIO,
        (IXAudio2VoiceCallback*)voiceCB_);
    if (FAILED(hr) || !source_)
    {
        Shutdown();
        return false;
    }

    // 环形缓冲区容量 1 秒
    ringCapacity_ = (size_t)sampleRate_ * channels_ * 2 * 1;
    ring_.assign(ringCapacity_, 0);
    writePos_ = 0;
    readPos_ = 0;
    playedSamples_ = 0;

    const size_t chunkBytes =
        ((size_t)sampleRate_ * channels_ * 2 * kChunkMs) / 1000;
    for (int i = 0; i < kNumSubmitBufs; ++i)
    {
        submitBufs_[i].data.assign(chunkBytes, 0);
        submitBufs_[i].inUse.store(false);
        submitBufs_[i].samples = 0;
    }

    feeding_ = true;
    paused_ = true;
    feedThread_ = std::thread(&AudioRenderer::FeedThread, this);

    return true;
}

// ---------------------------------------------------------------------------
void AudioRenderer::Shutdown()
{
    feeding_ = false;
    if (feedThread_.joinable())
        feedThread_.join();

    if (source_)
    {
        source_->Stop(0);
        source_->FlushSourceBuffers();
        source_->DestroyVoice();
        source_ = nullptr;
    }
    if (master_)
    {
        master_->DestroyVoice();
        master_ = nullptr;
    }
    if (xaudio_)
    {
        xaudio_->Release();
        xaudio_ = nullptr;
    }
    if (voiceCB_)
    {
        delete voiceCB_;
        voiceCB_ = nullptr;
    }

    ring_.clear();
    ringCapacity_ = 0;
    playedSamples_ = 0;
    paused_ = false;
}

// ---------------------------------------------------------------------------
void AudioRenderer::Play()
{
    if (!source_) return;
    if (paused_.exchange(false))
        source_->Start(0);
}

void AudioRenderer::Pause()
{
    if (!source_) return;
    if (!paused_.exchange(true))
        source_->Stop(0);
}

void AudioRenderer::Stop()
{
    if (!source_) return;
    source_->Stop(0);
    source_->FlushSourceBuffers();
    writePos_ = readPos_ = 0;
    playedSamples_ = 0;
    for (int i = 0; i < kNumSubmitBufs; ++i)
        submitBufs_[i].inUse.store(false);
}

void AudioRenderer::SetVolume(float v)
{
    if (source_) source_->SetVolume(v);
}

// ---------------------------------------------------------------------------
bool AudioRenderer::Write(const uint8_t* data, size_t bytes,
    const std::atomic<bool>& abort)
{
    size_t written = 0;
    while (written < bytes && !abort.load() && feeding_.load())
    {
        size_t chunkWritten = 0;
        {
            std::lock_guard<std::mutex> lk(ringMutex_);
            size_t used = (size_t)(writePos_.load() - readPos_.load());
            size_t avail = ringCapacity_ - used;
            if (avail > 0)
            {
                size_t toWrite = (std::min)(avail, bytes - written);
                uint64_t wp = writePos_.load();
                size_t   w = (size_t)(wp % ringCapacity_);
                size_t   first = (std::min)(toWrite, ringCapacity_ - w);
                std::memcpy(ring_.data() + w, data + written, first);
                if (toWrite > first)
                    std::memcpy(ring_.data(), data + written + first, toWrite - first);
                writePos_.store(wp + toWrite);
                chunkWritten = toWrite;
                written += toWrite;
            }
        }
        if (chunkWritten == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return written == bytes;
}

// ---------------------------------------------------------------------------
void AudioRenderer::FeedThread()
{
    const size_t bytesPerFrame = (size_t)channels_ * 2;
    const size_t chunkBytes = submitBufs_[0].data.size();

    while (feeding_.load())
    {
        if (paused_.load() || !source_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        SubmitBuf* freeBuf = nullptr;
        for (int i = 0; i < kNumSubmitBufs; ++i)
        {
            if (!submitBufs_[i].inUse.load(std::memory_order_acquire))
            {
                freeBuf = &submitBufs_[i];
                break;
            }
        }
        if (!freeBuf)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        size_t toRead = 0;
        {
            std::lock_guard<std::mutex> lk(ringMutex_);
            size_t used = (size_t)(writePos_.load() - readPos_.load());
            if (used > 0)
            {
                toRead = (std::min)(used, chunkBytes);
                uint64_t rp = readPos_.load();
                size_t   r = (size_t)(rp % ringCapacity_);
                size_t   first = (std::min)(toRead, ringCapacity_ - r);
                std::memcpy(freeBuf->data.data(), ring_.data() + r, first);
                if (toRead > first)
                    std::memcpy(freeBuf->data.data() + first, ring_.data(), toRead - first);
                readPos_.store(rp + toRead);
            }
        }

        if (toRead == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        freeBuf->samples = (uint32_t)(toRead / bytesPerFrame);
        freeBuf->inUse.store(true, std::memory_order_release);

        XAUDIO2_BUFFER xb = {};
        xb.AudioBytes = (UINT32)toRead;
        xb.pAudioData = freeBuf->data.data();
        xb.pContext = freeBuf;

        if (FAILED(source_->SubmitSourceBuffer(&xb)))
        {
            freeBuf->inUse.store(false, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}
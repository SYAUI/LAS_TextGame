#include "Decoder.h"
#include "AudioRenderer.h"

// ---------------------------------------------------------------------------
// DecodedFrame
// ---------------------------------------------------------------------------
DecodedFrame::DecodedFrame(AVFrame* f) : frame(f)
{
    if (f) pts = f->pts;
}

DecodedFrame::~DecodedFrame() { Reset(); }

DecodedFrame::DecodedFrame(DecodedFrame&& other) noexcept
    : frame(other.frame), pts(other.pts),
    ptsSec(other.ptsSec), durationSec(other.durationSec),
    isHardware(other.isHardware)
{
    other.frame = nullptr;
}

DecodedFrame& DecodedFrame::operator=(DecodedFrame&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        frame = other.frame;
        pts = other.pts;
        ptsSec = other.ptsSec;
        durationSec = other.durationSec;
        isHardware = other.isHardware;
        other.frame = nullptr;
    }
    return *this;
}

void DecodedFrame::Reset()
{
    if (frame)
    {
        av_frame_free(&frame);
        frame = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 硬解像素格式选择回调（只定义一次！）
// ---------------------------------------------------------------------------
static enum AVPixelFormat GetHwFormat(AVCodecContext* /*ctx*/,
    const enum AVPixelFormat* pixFmts)
{
    for (const enum AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p)
    {
        if (*p == AV_PIX_FMT_D3D11)
            return *p;
    }
    // 无 D3D11 支持则回退到第一个软件格式
    return pixFmts[0];
}

// ---------------------------------------------------------------------------
// VideoDecoder
// ---------------------------------------------------------------------------
VideoDecoder::VideoDecoder() {}
VideoDecoder::~VideoDecoder() { Close(); }

bool VideoDecoder::Open(ID3D11Device* device, const std::string& filePath)
{
    Close();
    device_ = device;
    error_.clear();

    int ret = avformat_open_input(&fmtCtx_, filePath.c_str(), nullptr, nullptr);
    if (ret < 0)
    {
        error_ = "无法打开文件: " + filePath;
        return false;
    }

    ret = avformat_find_stream_info(fmtCtx_, nullptr);
    if (ret < 0)
    {
        error_ = "无法读取流信息";
        Close();
        return false;
    }

    videoStreamIndex_ = av_find_best_stream(fmtCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioStreamIndex_ = av_find_best_stream(fmtCtx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (videoStreamIndex_ < 0 && audioStreamIndex_ < 0)
    {
        error_ = "文件不含音视频流";
        Close();
        return false;
    }

    // ---------------- 视频流 ----------------
    if (videoStreamIndex_ >= 0)
    {
        AVStream* vs = fmtCtx_->streams[videoStreamIndex_];
        const AVCodec* vcodec = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!vcodec)
        {
            error_ = "未找到视频解码器";
            Close();
            return false;
        }

        videoCodecCtx_ = avcodec_alloc_context3(vcodec);
        if (!videoCodecCtx_)
        {
            error_ = "分配视频解码器上下文失败";
            Close();
            return false;
        }
        avcodec_parameters_to_context(videoCodecCtx_, vs->codecpar);

        // D3D11VA 硬件上下文：alloc → set device → init 三步法
        AVBufferRef* hwCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (hwCtx)
        {
            AVHWDeviceContext* dc = (AVHWDeviceContext*)hwCtx->data;
            AVD3D11VADeviceContext* hc = (AVD3D11VADeviceContext*)dc->hwctx;

            // FFmpeg 会在释放时 Release 此设备，必须先 AddRef 平衡
            device_->AddRef();
            hc->device = device_;

            if (av_hwdevice_ctx_init(hwCtx) < 0)
            {
                OutputDebugStringA("[Decoder] av_hwdevice_ctx_init 失败，回退软解\n");
                av_buffer_unref(&hwCtx);
                hwCtx = nullptr;
            }
        }

        if (hwCtx)
        {
            videoCodecCtx_->hw_device_ctx = av_buffer_ref(hwCtx);
            videoCodecCtx_->get_format = GetHwFormat;
            hwDeviceCtx_ = hwCtx;
        }
        else
        {
            OutputDebugStringA("[Decoder] 硬解不可用，使用软解\n");
        }

        videoCodecCtx_->thread_count = 0;
        videoCodecCtx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

        if (avcodec_open2(videoCodecCtx_, vcodec, nullptr) < 0)
        {
            error_ = "打开视频解码器失败";
            Close();
            return false;
        }

        width_ = videoCodecCtx_->width;
        height_ = videoCodecCtx_->height;

        AVRational fr = vs->avg_frame_rate;
        fps_ = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 30.0;
    }

    duration_ = fmtCtx_->duration > 0
        ? fmtCtx_->duration / (double)AV_TIME_BASE : 0.0;

    // ---------------- 音频流 ----------------
    if (audioStreamIndex_ >= 0 && audioRenderer_)
    {
        AVStream* as = fmtCtx_->streams[audioStreamIndex_];
        const AVCodec* acodec = avcodec_find_decoder(as->codecpar->codec_id);
        if (acodec)
        {
            audioCodecCtx_ = avcodec_alloc_context3(acodec);
            if (audioCodecCtx_)
            {
                avcodec_parameters_to_context(audioCodecCtx_, as->codecpar);
                audioCodecCtx_->thread_count = 0;

                if (avcodec_open2(audioCodecCtx_, acodec, nullptr) >= 0)
                {
                    if (!audioRenderer_->Init(48000, 2))
                    {
                        OutputDebugStringA("[Decoder] AudioRenderer 初始化失败，仅视频模式\n");
                        avcodec_free_context(&audioCodecCtx_);
                        audioStreamIndex_ = -1;
                    }
                    else
                    {
                        // 重采样器：源格式 → S16 / 立体声 / 48kHz
#if LIBSWRESAMPLE_VERSION_MAJOR >= 4
                        AVChannelLayout outLayout;
                        av_channel_layout_default(&outLayout, 2);
                        int sret = swr_alloc_set_opts2(&swrCtx_,
                            &outLayout, AV_SAMPLE_FMT_S16, 48000,
                            &audioCodecCtx_->ch_layout,
                            audioCodecCtx_->sample_fmt,
                            audioCodecCtx_->sample_rate,
                            0, nullptr);
                        if (sret < 0 || swr_init(swrCtx_) < 0)
#else
                        swrCtx_ = swr_alloc_set_opts(nullptr,
                            AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 48000,
                            audioCodecCtx_->channel_layout,
                            audioCodecCtx_->sample_fmt,
                            audioCodecCtx_->sample_rate,
                            0, nullptr);
                        if (!swrCtx_ || swr_init(swrCtx_) < 0)
#endif
                        {
                            OutputDebugStringA("[Decoder] SwrContext 初始化失败，关闭音频\n");
                            if (swrCtx_) swr_free(&swrCtx_);
                            audioRenderer_->Shutdown();
                            avcodec_free_context(&audioCodecCtx_);
                            audioStreamIndex_ = -1;
                        }
                    }
                }
                else
                {
                    avcodec_free_context(&audioCodecCtx_);
                    audioStreamIndex_ = -1;
                }
            }
            else
            {
                audioStreamIndex_ = -1;
            }
        }
        else
        {
            audioStreamIndex_ = -1;
        }
    }

    // 状态复位
    abort_ = false;
    playing_ = false;
    demuxEof_ = false;
    firstAudioPts_ = -1.0;
    firstVideoPts_ = -1.0;
    playbackStartSet_ = false;

    // 启动线程
    demuxThread_ = std::thread(&VideoDecoder::DemuxLoop, this);
    if (videoStreamIndex_ >= 0)
        videoThread_ = std::thread(&VideoDecoder::VideoDecodeLoop, this);
    if (audioStreamIndex_ >= 0)
        audioThread_ = std::thread(&VideoDecoder::AudioDecodeLoop, this);

    return true;
}

void VideoDecoder::Close()
{
    abort_ = true;
    packetCv_.notify_all();
    queueCv_.notify_all();

    if (demuxThread_.joinable()) demuxThread_.join();
    if (videoThread_.joinable()) videoThread_.join();
    if (audioThread_.joinable()) audioThread_.join();

    // 清空包队列
    {
        std::lock_guard<std::mutex> lk(packetMutex_);
        while (!videoPackets_.empty()) { av_packet_free(&videoPackets_.front()); videoPackets_.pop(); }
        while (!audioPackets_.empty()) { av_packet_free(&audioPackets_.front()); audioPackets_.pop(); }
        demuxEof_ = false;
    }
    // 清空帧队列（在 codec 释放之前！）
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        while (!frameQueue_.empty()) frameQueue_.pop();
    }

    // 音频资源先释放
    if (audioRenderer_) audioRenderer_->Shutdown();
    if (swrCtx_) { swr_free(&swrCtx_); swrCtx_ = nullptr; }
    if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
    swsSrcFmt_ = -1;
    swsSrcW_ = swsSrcH_ = 0;

    // 再释放编解码器
    if (audioCodecCtx_) { avcodec_free_context(&audioCodecCtx_); audioCodecCtx_ = nullptr; }
    if (videoCodecCtx_) { avcodec_free_context(&videoCodecCtx_); videoCodecCtx_ = nullptr; }
    if (fmtCtx_) { avformat_close_input(&fmtCtx_);        fmtCtx_ = nullptr; }
    if (hwDeviceCtx_) { av_buffer_unref(&hwDeviceCtx_);        hwDeviceCtx_ = nullptr; }

    videoStreamIndex_ = audioStreamIndex_ = -1;
    width_ = height_ = 0;
    duration_ = fps_ = 0.0;
    abort_ = false;
    playing_ = false;
    device_ = nullptr;
}

void VideoDecoder::Play()
{
    playing_ = true;
    playbackStart_ = std::chrono::steady_clock::now();
    playbackStartSet_ = true;
    if (audioRenderer_) audioRenderer_->Play();
    queueCv_.notify_all();
    packetCv_.notify_all();
}

void VideoDecoder::Pause()
{
    playing_ = false;
    if (audioRenderer_) audioRenderer_->Pause();
    queueCv_.notify_all();
    packetCv_.notify_all();
}

void VideoDecoder::TogglePlay()
{
    if (playing_.load()) Pause();
    else                 Play();
}

bool VideoDecoder::PopFrame(DecodedFrame& out, int timeoutMs)
{
    std::unique_lock<std::mutex> lk(queueMutex_);
    if (timeoutMs > 0)
    {
        queueCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
            [this] { return !frameQueue_.empty() || abort_.load(); });
    }
    if (frameQueue_.empty()) return false;
    out = std::move(frameQueue_.front());
    frameQueue_.pop();
    queueCv_.notify_all();
    return true;
}

void VideoDecoder::ClearFrames()
{
    std::lock_guard<std::mutex> lk(queueMutex_);
    while (!frameQueue_.empty()) frameQueue_.pop();
    queueCv_.notify_all();
}

void VideoDecoder::PushFrameToQueue(AVFrame* f, double ptsSec,
    double durSec, bool isHw)
{
    std::unique_lock<std::mutex> lk(queueMutex_);
    while (!abort_.load() && frameQueue_.size() >= maxQueueSize_)
        queueCv_.wait_for(lk, std::chrono::milliseconds(100));

    if (abort_.load())
    {
        av_frame_free(&f);
        return;
    }

    DecodedFrame df(f);
    df.ptsSec = ptsSec;
    df.durationSec = durSec;
    df.isHardware = isHw;
    frameQueue_.push(std::move(df));
    queueCv_.notify_all();
}

AVPacket* VideoDecoder::PopVideoPacket(int timeoutMs, bool& eof)
{
    std::unique_lock<std::mutex> lk(packetMutex_);
    packetCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
        [this] { return !videoPackets_.empty() || demuxEof_ || abort_.load(); });
    eof = demuxEof_ && videoPackets_.empty();
    if (videoPackets_.empty()) return nullptr;
    AVPacket* p = videoPackets_.front();
    videoPackets_.pop();
    return p;
}

AVPacket* VideoDecoder::PopAudioPacket(int timeoutMs, bool& eof)
{
    std::unique_lock<std::mutex> lk(packetMutex_);
    packetCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
        [this] { return !audioPackets_.empty() || demuxEof_ || abort_.load(); });
    eof = demuxEof_ && audioPackets_.empty();
    if (audioPackets_.empty()) return nullptr;
    AVPacket* p = audioPackets_.front();
    audioPackets_.pop();
    return p;
}

// ---------------------------------------------------------------------------
// Demux 线程
// ---------------------------------------------------------------------------
void VideoDecoder::DemuxLoop()
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;

    while (!abort_.load())
    {
        if (!playing_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 背压：包队列满时等待
        {
            std::unique_lock<std::mutex> lk(packetMutex_);
            packetCv_.wait_for(lk, std::chrono::milliseconds(50), [this] {
                return abort_.load()
                    || (videoPackets_.size() < 20 && audioPackets_.size() < 30);
                });
            if (videoPackets_.size() >= 20 || audioPackets_.size() >= 30)
                continue;
        }

        int ret = av_read_frame(fmtCtx_, pkt);
        if (ret < 0)
        {
            std::lock_guard<std::mutex> lk(packetMutex_);
            demuxEof_ = true;
            packetCv_.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (pkt->stream_index == videoStreamIndex_)
        {
            AVPacket* clone = av_packet_clone(pkt);
            av_packet_unref(pkt);
            if (clone)
            {
                std::lock_guard<std::mutex> lk(packetMutex_);
                videoPackets_.push(clone);
                packetCv_.notify_all();
            }
        }
        else if (pkt->stream_index == audioStreamIndex_)
        {
            AVPacket* clone = av_packet_clone(pkt);
            av_packet_unref(pkt);
            if (clone)
            {
                std::lock_guard<std::mutex> lk(packetMutex_);
                audioPackets_.push(clone);
                packetCv_.notify_all();
            }
        }
        else
        {
            av_packet_unref(pkt);
        }
    }

    av_packet_free(&pkt);
}

// ---------------------------------------------------------------------------
// 视频解码线程
// ---------------------------------------------------------------------------
void VideoDecoder::VideoDecodeLoop()
{
    if (!videoCodecCtx_) return;

    AVStream* stream = fmtCtx_->streams[videoStreamIndex_];
    AVRational tb = stream->time_base;
    double frameDur = (fps_ > 0.0) ? (1.0 / fps_) : (1.0 / 30.0);
    int64_t lastPtsUs = 0;
    bool    eofSent = false;

    while (!abort_.load())
    {
        if (!playing_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        bool eof = false;
        AVPacket* pkt = PopVideoPacket(50, eof);

        if (pkt)
        {
            avcodec_send_packet(videoCodecCtx_, pkt);
            av_packet_free(&pkt);
        }
        else if (eof && !eofSent)
        {
            avcodec_send_packet(videoCodecCtx_, nullptr);
            eofSent = true;
        }
        else
        {
            continue;
        }

        // 取出所有可用帧
        while (true)
        {
            AVFrame* f = av_frame_alloc();
            if (!f) break;

            int ret = avcodec_receive_frame(videoCodecCtx_, f);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0)
            {
                av_frame_free(&f);
                break;
            }

            double pts = (f->pts != AV_NOPTS_VALUE)
                ? f->pts * av_q2d(tb)
                : lastPtsUs / (double)AV_TIME_BASE + frameDur;
            lastPtsUs = (int64_t)(pts * AV_TIME_BASE);

            if (firstVideoPts_.load() < 0.0)
                firstVideoPts_.store(pts);

            bool isHw = (f->format == AV_PIX_FMT_D3D11);

            if (!isHw)
            {
                // 软解回退：转 BGRA
                if (!swsCtx_ || swsSrcFmt_ != f->format ||
                    swsSrcW_ != f->width || swsSrcH_ != f->height)
                {
                    if (swsCtx_) sws_freeContext(swsCtx_);
                    swsCtx_ = sws_getContext(
                        f->width, f->height, (AVPixelFormat)f->format,
                        f->width, f->height, AV_PIX_FMT_BGRA,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                    swsSrcFmt_ = f->format;
                    swsSrcW_ = f->width;
                    swsSrcH_ = f->height;
                }

                if (!swsCtx_)
                {
                    av_frame_free(&f);
                    continue;
                }

                AVFrame* bgra = av_frame_alloc();
                if (!bgra) { av_frame_free(&f); break; }

                bgra->format = AV_PIX_FMT_BGRA;
                bgra->width = f->width;
                bgra->height = f->height;

                if (av_frame_get_buffer(bgra, 32) < 0)
                {
                    av_frame_free(&bgra);
                    av_frame_free(&f);
                    continue;
                }

                sws_scale(swsCtx_, f->data, f->linesize, 0, f->height,
                    bgra->data, bgra->linesize);

                bgra->pts = f->pts;
                av_frame_free(&f);
                f = bgra;
            }

            PushFrameToQueue(f, pts, frameDur, isHw);
        }
    }
}

// ---------------------------------------------------------------------------
// 音频解码线程
// ---------------------------------------------------------------------------
void VideoDecoder::AudioDecodeLoop()
{
    if (!audioCodecCtx_ || !audioRenderer_ || !swrCtx_) return;

    AVStream* stream = fmtCtx_->streams[audioStreamIndex_];
    AVRational tb = stream->time_base;

    const int outChannels = 2;
    const int bytesPerFrame = outChannels * 2;

    uint8_t* pcmBuf = nullptr;
    int      pcmBufBytes = 0;
    bool     eofSent = false;

    while (!abort_.load())
    {
        if (!playing_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 环形缓冲超过 0.5 秒就等一下
        if (audioRenderer_->BufferedBytes() >
            (size_t)audioRenderer_->SampleRate() * bytesPerFrame / 2)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        bool eof = false;
        AVPacket* pkt = PopAudioPacket(50, eof);

        if (pkt)
        {
            avcodec_send_packet(audioCodecCtx_, pkt);
            av_packet_free(&pkt);
        }
        else if (eof && !eofSent)
        {
            avcodec_send_packet(audioCodecCtx_, nullptr);
            eofSent = true;
        }
        else
        {
            continue;
        }

        while (true)
        {
            AVFrame* f = av_frame_alloc();
            if (!f) break;

            int ret = avcodec_receive_frame(audioCodecCtx_, f);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0)
            {
                av_frame_free(&f);
                break;
            }

            if (firstAudioPts_.load() < 0.0 && f->pts != AV_NOPTS_VALUE)
                firstAudioPts_.store(f->pts * av_q2d(tb));

            int outSamples = swr_get_out_samples(swrCtx_, f->nb_samples);
            if (outSamples < 0) outSamples = f->nb_samples + 256;

            int neededBytes = outSamples * bytesPerFrame;
            if (neededBytes > pcmBufBytes)
            {
                if (pcmBuf) av_freep(&pcmBuf);
                if (av_samples_alloc(&pcmBuf, nullptr, outChannels, outSamples,
                    AV_SAMPLE_FMT_S16, 0) < 0)
                {
                    pcmBuf = nullptr;
                    pcmBufBytes = 0;
                    av_frame_free(&f);
                    break;
                }
                pcmBufBytes = neededBytes;
            }

            uint8_t* out = pcmBuf;
            int converted = swr_convert(swrCtx_, &out, outSamples,
                (const uint8_t**)f->data, f->nb_samples);
            if (converted > 0)
            {
                size_t bytes = (size_t)converted * bytesPerFrame;
                audioRenderer_->Write(pcmBuf, bytes, abort_);
            }

            av_frame_free(&f);
        }
    }

    if (pcmBuf) av_freep(&pcmBuf);
}

// ---------------------------------------------------------------------------
// 主时钟
// ---------------------------------------------------------------------------
double VideoDecoder::MasterClock() const
{
    // 优先音频时钟
    if (audioStreamIndex_ >= 0 && audioRenderer_ && audioRenderer_->IsReady())
    {
        double base = firstAudioPts_.load();
        if (base >= 0.0)
        {
            double clock = base + audioRenderer_->PlayedSeconds();

            // 下限保护：音频欠载时画面不至于停滞
            if (playbackStartSet_.load())
            {
                double vbase = firstVideoPts_.load();
                if (vbase < 0.0) vbase = base;
                auto elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - playbackStart_).count();
                double wall = vbase + elapsed;
                if (clock < wall - 0.5) clock = wall - 0.5;
            }
            return clock;
        }
    }

    // 回退系统时钟
    double base = firstVideoPts_.load();
    if (base < 0.0) base = 0.0;
    if (!playbackStartSet_.load()) return base;

    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - playbackStart_).count();
    return base + elapsed;
}
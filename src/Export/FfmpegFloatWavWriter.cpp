#include "Export/FfmpegFloatWavWriter.hpp"
#include "Export/ExportScratchIo.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#include <cstring>
#include <limits>
#include <stdexcept>

namespace AveMediaBridge::Export {
namespace {
void check(int value, const char* action) { if (value < 0) throw std::runtime_error(action); }
class FloatWavWriter final : public IStreamingPcmWriter {
public:
    FloatWavWriter(const Path& scratchPath, const AMBE_InputV1& input, bool forceRf64,
        ExportScratchIo::Fault scratchFault)
        : scratch_(scratchPath, scratchFault), channels_(input.channels) {
      try {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PCM_F32LE);
        if (!codec) throw std::runtime_error("pcm_f32le encoder unavailable");
        check(avformat_alloc_output_context2(&format_, nullptr, "wav", nullptr), "WAV muxer unavailable");
        if (!format_) throw std::runtime_error("WAV context unavailable");
        format_->pb = scratch_.context();
        format_->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_BITEXACT;
        stream_ = avformat_new_stream(format_, nullptr);
        if (!stream_) throw std::bad_alloc();
        codec_ = avcodec_alloc_context3(codec);
        if (!codec_) throw std::bad_alloc();
        codec_->codec_id = AV_CODEC_ID_PCM_F32LE;
        codec_->codec_type = AVMEDIA_TYPE_AUDIO;
        codec_->sample_fmt = AV_SAMPLE_FMT_FLT;
        codec_->sample_rate = static_cast<int>(input.sampleRate);
        codec_->time_base = AVRational{1, static_cast<int>(input.sampleRate)};
        av_channel_layout_default(&codec_->ch_layout, static_cast<int>(input.channels));
        check(avcodec_open2(codec_, codec, nullptr), "PCM encoder open failed");
        check(avcodec_parameters_from_context(stream_->codecpar, codec_), "stream parameters failed");
        stream_->time_base = codec_->time_base;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rf64", forceRf64 ? "always" : "auto", 0);
        const int header = avformat_write_header(format_, &options);
        av_dict_free(&options);
        check(header, "WAV header failed");
      } catch (...) {
        if (format_) avformat_free_context(format_);
        if (codec_) avcodec_free_context(&codec_);
        throw;
      }
    }
    ~FloatWavWriter() override { abort(); }
    void write(const float* samples, uint32_t frames) override {
        if (finished_ || !samples || frames == 0) throw std::runtime_error("invalid writer state");
        const uint64_t count = static_cast<uint64_t>(frames) * channels_;
        if (count > AMBE_MAX_BLOCK_BYTES / sizeof(float)) throw std::runtime_error("block too large");
        AVFrame* frame = av_frame_alloc();
        if (!frame) throw std::bad_alloc();
        try {
            frame->nb_samples = static_cast<int>(frames);
            frame->format = AV_SAMPLE_FMT_FLT;
            frame->sample_rate = codec_->sample_rate;
            check(av_channel_layout_copy(&frame->ch_layout, &codec_->ch_layout), "frame channel layout failed");
            check(av_frame_get_buffer(frame, 1), "frame buffer failed");
            std::memcpy(frame->data[0], samples, static_cast<size_t>(count * sizeof(float)));
            frame->pts = static_cast<int64_t>(submitted_);
            check(avcodec_send_frame(codec_, frame), "PCM encoder submit failed");
            submitted_ += frames;
            drain();
        } catch (...) { av_frame_free(&frame); throw; }
        av_frame_free(&frame);
    }
    uint64_t finish() override {
        if (finished_) throw std::runtime_error("already finished");
        check(avcodec_send_frame(codec_, nullptr), "PCM encoder drain failed");
        drain();
        if (encoded_ != submitted_) throw std::runtime_error("PCM frame mismatch");
        const int trailer=av_write_trailer(format_);
        if (trailer<0 && (scratch_.failed() || scratch_.context()->error))
            throw WriterIoFailure("WAV trailer I/O failed");
        check(trailer, "WAV trailer failed");
        if (scratch_.failed() || scratch_.context()->error) throw WriterIoFailure("WAV I/O failed");
        try { scratch_.flushAndClose(); }
        catch (const std::runtime_error&) { throw WriterIoFailure("WAV flush/close failed"); }
        finished_ = true;
        return encoded_;
    }
    void abort() noexcept override {
        if (format_) { avformat_free_context(format_); format_ = nullptr; }
        if (codec_) avcodec_free_context(&codec_);
        scratch_.abortClose();
    }
private:
    void drain() {
        AVPacket* packet = av_packet_alloc();
        if (!packet) throw std::bad_alloc();
        try {
            for (;;) {
                const int rc = avcodec_receive_packet(codec_, packet);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
                check(rc, "PCM encoder receive failed");
                if (packet->size < 0 || packet->size % (channels_ * sizeof(float)) != 0)
                    throw std::runtime_error("PCM packet is not whole frames");
                encoded_ += static_cast<uint64_t>(packet->size) / (channels_ * sizeof(float));
                av_packet_rescale_ts(packet, codec_->time_base, stream_->time_base);
                packet->stream_index = stream_->index;
                const int written=av_interleaved_write_frame(format_, packet);
                if (written<0 && (scratch_.failed() || scratch_.context()->error))
                    throw WriterIoFailure("WAV packet I/O failed");
                check(written, "WAV packet write failed");
                if (scratch_.failed() || scratch_.context()->error)
                    throw WriterIoFailure("WAV packet I/O failed");
                av_packet_unref(packet);
            }
        } catch (...) { av_packet_free(&packet); throw; }
        av_packet_free(&packet);
    }
    ExportScratchIo scratch_;
    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVStream* stream_ = nullptr;
    uint32_t channels_ = 0;
    uint64_t submitted_ = 0;
    uint64_t encoded_ = 0;
    bool finished_ = false;
};
}
bool floatWavWriterAvailable() noexcept {
    return avcodec_find_encoder(AV_CODEC_ID_PCM_F32LE) && av_guess_format("wav", nullptr, nullptr);
}
std::unique_ptr<IStreamingPcmWriter> makeFloatWavWriter(const Path& path, const AMBE_InputV1& input,
    bool forceRf64ForTest, ExportScratchIo::Fault scratchFaultForTest) {
    if (!floatWavWriterAvailable()) throw std::runtime_error("writer runtime unavailable");
    return std::make_unique<FloatWavWriter>(path, input, forceRf64ForTest, scratchFaultForTest);
}
}

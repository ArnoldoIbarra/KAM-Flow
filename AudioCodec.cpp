// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

#include "AudioCodec.h"

#include <opus.h>

#include <algorithm>
#include <cstring>

namespace AudioCodec {

    namespace {
        /// Maximum encoded Opus packet buffer size in bytes for a single 10ms frame.
        constexpr opus_int32 kMaxOpusPacketBytes = 1200;
        /// Backward sequence delta beyond which a capture stream restart is assumed.
        constexpr int32_t kSeqRestartBackwardThreshold = -50;
        /// Forward sequence jump beyond which sequence state is resynchronized instead of concealed.
        constexpr int32_t kSeqRestartForwardThreshold = 500;
        /// Simulated packet loss percentage fed to Opus encoder to control In-Band FEC LBRR redundancy.
        constexpr int kOpusExpectedLossPercent = 5;
        /// Encoder computational complexity (0-10); 5 balances high quality with low CPU overhead.
        constexpr int kOpusComplexity = 5;
    }

    /**
     * @brief Checks whether the given PCM stream parameters are natively supported by libopus.
     * @param sampleRate Audio sample rate in Hz.
     * @param channels Number of audio channels.
     * @param bitDepth Input/output PCM bit depth (16-bit int or 32-bit float).
     * @return True when libopus supports encoding and decoding this format cleanly.
     */
    bool IsOpusFormatSupported(uint32_t sampleRate, uint16_t channels, uint16_t bitDepth) {
        if (channels != 1 && channels != 2) {
            return false;
        }
        if (bitDepth != 16 && bitDepth != 32) {
            return false;
        }
        return sampleRate == 8000 ||
               sampleRate == 12000 ||
               sampleRate == 16000 ||
               sampleRate == 24000 ||
               sampleRate == 48000;
    }

    /**
     * @brief Selects the wire codec ID based on Server preference and stream compatibility.
     * @param preferOpus True if the Server requests Opus compression.
     * @param sampleRate Stream sample rate in Hz.
     * @param channels Stream channel count.
     * @param bitDepth Stream PCM bit depth.
     * @return kAudioCodecOpus when preferred and supported; otherwise kAudioCodecPcm.
     */
    uint8_t ResolveEffectiveAudioCodec(bool preferOpus, uint32_t sampleRate, uint16_t channels, uint16_t bitDepth) {
        if (preferOpus && IsOpusFormatSupported(sampleRate, channels, bitDepth)) {
            return kAudioCodecOpus;
        }
        return kAudioCodecPcm;
    }

    /**
     * @brief Computes the largest frame-aligned PCM chunk size safe against IPv4 fragmentation.
     * @param blockAlign Byte size of one interleaved PCM audio frame.
     * @param maxPayloadBytes Maximum desired raw PCM payload size per UDP datagram.
     * @return Frame-aligned chunk size in bytes, always at least blockAlign when non-zero.
     */
    size_t ComputeMtuSafePcmChunkBytes(uint32_t blockAlign, size_t maxPayloadBytes) {
        if (blockAlign == 0) {
            return maxPayloadBytes;
        }
        if (maxPayloadBytes < blockAlign) {
            return blockAlign;
        }
        return (maxPayloadBytes / blockAlign) * blockAlign;
    }

    /**
     * @brief Constructs an uninitialized OpusStreamEncoder.
     */
    OpusStreamEncoder::OpusStreamEncoder() = default;

    /**
     * @brief Destroys the OpusStreamEncoder and frees libopus state.
     */
    OpusStreamEncoder::~OpusStreamEncoder() {
        Shutdown();
    }

    /**
     * @brief Allocates and configures a libopus encoder for 10ms frames and In-Band FEC.
     * @param sampleRate Sample rate in Hz (must be Opus-compatible).
     * @param bitDepth Source PCM bit depth (16 or 32).
     * @param channels Channel count (1 or 2).
     * @param isVoice True for microphone/speech (VOIP profile), false for desktop audio (AUDIO profile).
     * @return True if initialization succeeded; false if format is unsupported or allocation failed.
     */
    bool OpusStreamEncoder::Initialize(uint32_t sampleRate, uint16_t bitDepth, uint16_t channels, bool isVoice) {
        Shutdown();
        if (!IsOpusFormatSupported(sampleRate, channels, bitDepth)) {
            return false;
        }

        int err = OPUS_OK;
        const int application = isVoice ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO;
        ::OpusEncoder* enc = opus_encoder_create(
            static_cast<opus_int32>(sampleRate),
            static_cast<int>(channels),
            application,
            &err);
        if (!enc || err != OPUS_OK) {
            if (enc) {
                opus_encoder_destroy(enc);
            }
            return false;
        }

        const opus_int32 bitrate = isVoice
            ? (channels == 1 ? 64000 : 96000)
            : (channels == 1 ? 64000 : 128000);
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(kOpusComplexity));
        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(kOpusExpectedLossPercent));
        opus_encoder_ctl(enc, OPUS_SET_DTX(0));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(isVoice ? OPUS_SIGNAL_VOICE : OPUS_SIGNAL_MUSIC));

        m_encoder = enc;
        m_sampleRate = sampleRate;
        m_bitDepth = bitDepth;
        m_channels = channels;
        m_frameSize = static_cast<uint16_t>(sampleRate / 100);
        m_silenceSkippedPending = false;
        m_pcmFifo.clear();
        m_pcmFifo.reserve(static_cast<size_t>(m_frameSize) * m_channels * 4);
        m_encodeScratch.resize(kMaxOpusPacketBytes);
        return true;
    }

    /**
     * @brief Frees the libopus encoder instance and resets stored format metadata.
     * @return void
     */
    void OpusStreamEncoder::Shutdown() {
        if (m_encoder) {
            opus_encoder_destroy(m_encoder);
            m_encoder = nullptr;
        }
        m_sampleRate = 0;
        m_bitDepth = 0;
        m_channels = 0;
        m_frameSize = 0;
        m_silenceSkippedPending = false;
        m_pcmFifo.clear();
        m_encodeScratch.clear();
    }

    /**
     * @brief Clears pending FIFO samples and resets Opus DSP state.
     * @param resetSequence True to reset audioSeq back to zero; false to preserve monotonic sequence IDs.
     * @return void
     */
    void OpusStreamEncoder::Reset(bool resetSequence) {
        m_pcmFifo.clear();
        m_silenceSkippedPending = false;
        if (resetSequence) {
            m_nextSeq = 0;
        }
        if (m_encoder) {
            opus_encoder_ctl(m_encoder, OPUS_RESET_STATE);
        }
    }

    /**
     * @brief Returns and increments the monotonic per-stream audio sequence counter.
     * @return Sequence number assigned to the outgoing packet.
     */
    uint32_t OpusStreamEncoder::NextSequence() {
        return m_nextSeq++;
    }

    /**
     * @brief Records that active encoding was bypassed during silence so resumed audio clears stale MDCT memory.
     * @return void
     */
    void OpusStreamEncoder::NotifySilenceSkipped() {
        m_pcmFifo.clear();
        m_silenceSkippedPending = true;
    }

    /**
     * @brief Buffers interleaved PCM input and encodes every full 10ms frame into Opus packets.
     * @param pcmData Pointer to interleaved 16-bit integer or 32-bit float PCM samples.
     * @param dataBytes Byte count of pcmData.
     * @param outPackets Vector appended with newly encoded 10ms Opus packets.
     * @return True if initialized and encoded cleanly; false otherwise.
     */
    bool OpusStreamEncoder::PushPcmAndEncode(const void* pcmData, size_t dataBytes, std::vector<EncodedOpusPacket>& outPackets) {
        if (!m_encoder || m_frameSize == 0 || m_channels == 0) {
            return false;
        }
        if (!pcmData || dataBytes == 0) {
            return true;
        }

        if (m_bitDepth == 32) {
            const size_t sampleCount = dataBytes / sizeof(float);
            const float* src = static_cast<const float*>(pcmData);
            m_pcmFifo.insert(m_pcmFifo.end(), src, src + sampleCount);
        } else if (m_bitDepth == 16) {
            const size_t sampleCount = dataBytes / sizeof(int16_t);
            const int16_t* src = static_cast<const int16_t*>(pcmData);
            m_pcmFifo.reserve(m_pcmFifo.size() + sampleCount);
            for (size_t i = 0; i < sampleCount; ++i) {
                m_pcmFifo.push_back(static_cast<float>(src[i]) / 32768.0f);
            }
        } else {
            return false;
        }

        const size_t samplesPerFrame = static_cast<size_t>(m_frameSize) * m_channels;
        size_t readOffset = 0;

        while (m_pcmFifo.size() - readOffset >= samplesPerFrame) {
            if (m_silenceSkippedPending) {
                opus_encoder_ctl(m_encoder, OPUS_RESET_STATE);
                m_silenceSkippedPending = false;
            }

            const opus_int32 encodedBytes = opus_encode_float(
                m_encoder,
                m_pcmFifo.data() + readOffset,
                static_cast<int>(m_frameSize),
                m_encodeScratch.data(),
                kMaxOpusPacketBytes);
            if (encodedBytes < 0) {
                m_pcmFifo.erase(m_pcmFifo.begin(), m_pcmFifo.begin() + static_cast<std::ptrdiff_t>(readOffset));
                return false;
            }

            EncodedOpusPacket pkt;
            pkt.audioSeq = NextSequence();
            pkt.frameCount = m_frameSize;
            pkt.flags = 0;
            pkt.opusBytes.assign(m_encodeScratch.begin(), m_encodeScratch.begin() + encodedBytes);
            outPackets.push_back(std::move(pkt));

            readOffset += samplesPerFrame;
        }

        if (readOffset > 0) {
            m_pcmFifo.erase(m_pcmFifo.begin(), m_pcmFifo.begin() + static_cast<std::ptrdiff_t>(readOffset));
        }
        return true;
    }

    /**
     * @brief Constructs an uninitialized OpusStreamDecoder.
     */
    OpusStreamDecoder::OpusStreamDecoder() = default;

    /**
     * @brief Destroys the OpusStreamDecoder and frees libopus state.
     */
    OpusStreamDecoder::~OpusStreamDecoder() {
        Shutdown();
    }

    /**
     * @brief Allocates and configures a libopus decoder matching the target WASAPI mix format.
     * @param sampleRate Sample rate in Hz.
     * @param bitDepth Output PCM bit depth (16 or 32).
     * @param channels Output channel count (1 or 2).
     * @return True if initialization succeeded; false if unsupported or allocation failed.
     */
    bool OpusStreamDecoder::Initialize(uint32_t sampleRate, uint16_t bitDepth, uint16_t channels) {
        Shutdown();
        if (!IsOpusFormatSupported(sampleRate, channels, bitDepth)) {
            return false;
        }

        int err = OPUS_OK;
        ::OpusDecoder* dec = opus_decoder_create(
            static_cast<opus_int32>(sampleRate),
            static_cast<int>(channels),
            &err);
        if (!dec || err != OPUS_OK) {
            if (dec) {
                opus_decoder_destroy(dec);
            }
            return false;
        }

        m_decoder = dec;
        m_sampleRate = sampleRate;
        m_bitDepth = bitDepth;
        m_channels = channels;
        m_frameSize = static_cast<uint16_t>(sampleRate / 100);
        m_expectedSeq = 0;
        m_hasExpectedSeq = false;
        m_inSilence = false;
        // Allocate enough scratch for up to 120ms (max valid Opus packet duration) to avoid reallocations.
        m_decodeScratch.resize(static_cast<size_t>(sampleRate / 100) * 12 * channels);
        return true;
    }

    /**
     * @brief Frees the libopus decoder instance and clears sequence state.
     * @return void
     */
    void OpusStreamDecoder::Shutdown() {
        if (m_decoder) {
            opus_decoder_destroy(m_decoder);
            m_decoder = nullptr;
        }
        m_sampleRate = 0;
        m_bitDepth = 0;
        m_channels = 0;
        m_frameSize = 0;
        m_expectedSeq = 0;
        m_hasExpectedSeq = false;
        m_inSilence = false;
        m_decodeScratch.clear();
    }

    /**
     * @brief Resets Opus decoder DSP memory and optionally resets sequence tracking.
     * @param resetSequence True to forget the next expected audio sequence number.
     * @return void
     */
    void OpusStreamDecoder::Reset(bool resetSequence) {
        if (m_decoder) {
            opus_decoder_ctl(m_decoder, OPUS_RESET_STATE);
        }
        m_inSilence = false;
        if (resetSequence) {
            m_expectedSeq = 0;
            m_hasExpectedSeq = false;
        }
    }

    /**
     * @brief Checks whether the decoder is already configured for the specified stream format.
     * @param sampleRate Sample rate in Hz.
     * @param bitDepth PCM bit depth (16 or 32).
     * @param channels Channel count (1 or 2).
     * @return True if initialized and matching all format parameters.
     */
    bool OpusStreamDecoder::MatchesFormat(uint32_t sampleRate, uint16_t bitDepth, uint16_t channels) const {
        return m_decoder != nullptr &&
               m_sampleRate == sampleRate &&
               m_bitDepth == bitDepth &&
               m_channels == channels;
    }

    /**
     * @brief Appends decoded float samples to outPcmBytes in either 32-bit float or clamped 16-bit int format.
     * @param samples Interleaved decoded float samples.
     * @param frameCount Number of frames in samples.
     * @param outPcmBytes Destination byte buffer appended with converted PCM bytes.
     * @return void
     */
    void OpusStreamDecoder::AppendDecodedFloatFrames(const float* samples, uint16_t frameCount, std::vector<uint8_t>& outPcmBytes) const {
        const size_t totalSamples = static_cast<size_t>(frameCount) * m_channels;
        if (totalSamples == 0) {
            return;
        }

        if (m_bitDepth == 32) {
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(samples);
            outPcmBytes.insert(outPcmBytes.end(), raw, raw + (totalSamples * sizeof(float)));
        } else if (m_bitDepth == 16) {
            const size_t oldSize = outPcmBytes.size();
            outPcmBytes.resize(oldSize + (totalSamples * sizeof(int16_t)));
            int16_t* dst = reinterpret_cast<int16_t*>(outPcmBytes.data() + oldSize);
            for (size_t i = 0; i < totalSamples; ++i) {
                const float clamped = (std::max)(-1.0f, (std::min)(1.0f, samples[i]));
                const float scaled = clamped * 32767.0f;
                dst[i] = static_cast<int16_t>(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
            }
        }
    }

    /**
     * @brief Decodes one Opus or synthetic silence packet, applying In-Band FEC and bounded PLC on gaps.
     * @param flags AudioPacketHeader flags (e.g., kAudioFlagSilence).
     * @param frameCount Expected PCM frames per packet (defaults to 10ms frame size if 0).
     * @param audioSeq Monotonic sequence number from AudioPacketHeader.
     * @param opusPayload Pointer to encoded Opus frame bytes (nullptr allowed for silence packets).
     * @param payloadBytes Byte size of opusPayload.
     * @param outPcmBytes Output buffer appended with decoded (and any concealed) PCM bytes.
     * @param outStats Telemetry counters incremented for FEC, PLC, stale drops, and stream resyncs.
     * @return True if one or more frames were decoded into outPcmBytes; false if stale/invalid.
     */
    bool OpusStreamDecoder::DecodePacket(uint8_t flags,
                                         uint16_t frameCount,
                                         uint32_t audioSeq,
                                         const uint8_t* opusPayload,
                                         size_t payloadBytes,
                                         std::vector<uint8_t>& outPcmBytes,
                                         OpusDecodeStats& outStats) {
        if (!m_decoder || m_frameSize == 0 || m_channels == 0) {
            return false;
        }
        const bool isSilenceFastPath = ((flags & kAudioFlagSilence) != 0) && (payloadBytes == 0);
        if (!isSilenceFastPath && (!opusPayload || payloadBytes == 0)) {
            return false;
        }

        if (!m_hasExpectedSeq) {
            m_expectedSeq = audioSeq;
            m_hasExpectedSeq = true;
        } else {
            const int32_t diff = static_cast<int32_t>(audioSeq - m_expectedSeq);
            if (diff < kSeqRestartBackwardThreshold || diff > kSeqRestartForwardThreshold) {
                // Capture thread restarted or stream resumed after a multi-second jump; resync cleanly.
                Reset(false);
                m_expectedSeq = audioSeq;
                ++outStats.resyncCount;
            } else if (diff < 0) {
                ++outStats.staleDropped;
                return false;
            } else if (diff > 0) {
                // Synthesize missing frames capped at kMaxOpusPlcGapFrames: PLC for earlier misses,
                // and In-Band FEC (decode_fec=1) from the current active packet for the immediately preceding miss.
                const uint32_t gapToSynthesize = (std::min)(static_cast<uint32_t>(diff), kMaxOpusPlcGapFrames);
                const uint32_t plcOnlyCount = gapToSynthesize - 1;

                for (uint32_t i = 0; i < plcOnlyCount; ++i) {
                    const int n = opus_decode_float(
                        m_decoder,
                        nullptr,
                        0,
                        m_decodeScratch.data(),
                        static_cast<int>(m_frameSize),
                        0);
                    if (n > 0) {
                        AppendDecodedFloatFrames(m_decodeScratch.data(), static_cast<uint16_t>(n), outPcmBytes);
                        ++outStats.plcFrames;
                    }
                }

                const bool canUseFec = ((flags & kAudioFlagSilence) == 0) &&
                                       (opusPayload != nullptr) &&
                                       (payloadBytes > 0);
                if (canUseFec) {
                    const int n = opus_decode_float(
                        m_decoder,
                        opusPayload,
                        static_cast<opus_int32>(payloadBytes),
                        m_decodeScratch.data(),
                        static_cast<int>(m_frameSize),
                        1);
                    if (n > 0) {
                        AppendDecodedFloatFrames(m_decodeScratch.data(), static_cast<uint16_t>(n), outPcmBytes);
                        ++outStats.fecFrames;
                    }
                } else {
                    const int n = opus_decode_float(
                        m_decoder,
                        nullptr,
                        0,
                        m_decodeScratch.data(),
                        static_cast<int>(m_frameSize),
                        0);
                    if (n > 0) {
                        AppendDecodedFloatFrames(m_decodeScratch.data(), static_cast<uint16_t>(n), outPcmBytes);
                        ++outStats.plcFrames;
                    }
                }
            }
        }

        if ((flags & kAudioFlagSilence) != 0 && payloadBytes == 0) {
            const uint16_t silenceFrames = (frameCount > 0) ? frameCount : m_frameSize;
            const size_t silenceBytes = static_cast<size_t>(silenceFrames) * m_channels * (m_bitDepth / 8);
            outPcmBytes.insert(outPcmBytes.end(), silenceBytes, 0);
            if (!m_inSilence) {
                opus_decoder_ctl(m_decoder, OPUS_RESET_STATE);
                m_inSilence = true;
            }
            m_expectedSeq = audioSeq + 1;
            return true;
        }

        if (!opusPayload || payloadBytes == 0) {
            return false;
        }

        m_inSilence = false;
        const int maxDecodeFrames = static_cast<int>(m_decodeScratch.size() / m_channels);
        const int decoded = opus_decode_float(
            m_decoder,
            opusPayload,
            static_cast<opus_int32>(payloadBytes),
            m_decodeScratch.data(),
            maxDecodeFrames,
            0);
        if (decoded <= 0) {
            return false;
        }

        AppendDecodedFloatFrames(m_decodeScratch.data(), static_cast<uint16_t>(decoded), outPcmBytes);
        m_expectedSeq = audioSeq + 1;
        return true;
    }

} // namespace AudioCodec

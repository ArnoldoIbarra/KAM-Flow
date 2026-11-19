// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Simple Keyboard and Mouse sharing with Audio Mixing 
// =============================================================================

/// @file AudioCodec.h
/// Portable Opus codec wrapper, MTU-safe PCM chunk sizing, and FEC/PLC loss recovery.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace AudioCodec {

    /// Wire codec ID for uncompressed PCM audio frames.
    constexpr uint8_t kAudioCodecPcm = 0;
    /// Wire codec ID for Opus-compressed audio frames.
    constexpr uint8_t kAudioCodecOpus = 1;
    /// Packet flag indicating pure digital silence (no compressed bytes required).
    constexpr uint8_t kAudioFlagSilence = 0x01;

    /// Maximum raw PCM payload bytes per UDP audio packet to stay well below IPv4 1500B MTU.
    constexpr size_t kMaxSafePcmPayloadBytes = 1280;
    /// Maximum consecutive lost 10ms frames synthesized via Opus PLC per incoming packet.
    constexpr uint32_t kMaxOpusPlcGapFrames = 4;

    /// Encoded 10ms Opus audio packet ready for network framing.
    struct EncodedOpusPacket {
        uint32_t audioSeq = 0;
        uint16_t frameCount = 0;
        uint8_t flags = 0;
        std::vector<uint8_t> opusBytes;
    };

    /// Loss-recovery and sequence diagnostics emitted during decoding.
    struct OpusDecodeStats {
        uint32_t fecFrames = 0;
        uint32_t plcFrames = 0;
        uint32_t staleDropped = 0;
        uint32_t resyncCount = 0;
    };

    /// Returns true if libopus supports the given sample rate, channel count, and bit depth.
    bool IsOpusFormatSupported(uint32_t sampleRate, uint16_t channels, uint16_t bitDepth);

    /// Resolves the effective wire codec, falling back to PCM if the format is unsupported by Opus.
    uint8_t ResolveEffectiveAudioCodec(bool preferOpus, uint32_t sampleRate, uint16_t channels, uint16_t bitDepth);

    /// Computes the largest whole-frame PCM payload size (in bytes) that fits within maxPayloadBytes.
    size_t ComputeMtuSafePcmChunkBytes(uint32_t blockAlign, size_t maxPayloadBytes = kMaxSafePcmPayloadBytes);

    /// RAII 10ms streaming Opus encoder supporting 16-bit integer and 32-bit float PCM inputs.
    class OpusStreamEncoder {
    public:
        /// Constructs an uninitialized Opus encoder.
        OpusStreamEncoder();
        /// Releases underlying libopus encoder state.
        ~OpusStreamEncoder();

        OpusStreamEncoder(const OpusStreamEncoder&) = delete;
        OpusStreamEncoder& operator=(const OpusStreamEncoder&) = delete;

        /// Configures the Opus encoder for the specified PCM stream format and usage profile.
        bool Initialize(uint32_t sampleRate, uint16_t bitDepth, uint16_t channels, bool isVoice = false);
        /// Destroys the libopus encoder and clears internal staging buffers.
        void Shutdown();
        /// Clears buffered partial frames and resets encoder DSP state.
        void Reset(bool resetSequence = false);

        /// Returns true if the encoder is ready to process audio.
        bool IsInitialized() const { return m_encoder != nullptr; }
        /// Returns the configured sample rate in Hz.
        uint32_t GetSampleRate() const { return m_sampleRate; }
        /// Returns the configured channel count (1 or 2).
        uint16_t GetChannels() const { return m_channels; }
        /// Returns the configured PCM input bit depth (16 or 32).
        uint16_t GetBitDepth() const { return m_bitDepth; }
        /// Returns the 10ms Opus frame size in samples per channel.
        uint16_t GetFrameSizeSamples() const { return m_frameSize; }

        /// Allocates the next monotonic audio sequence number for audio or silence packets.
        uint32_t NextSequence();
        /// Marks that silence packets bypassed encoding so the next active frame clears stale state.
        void NotifySilenceSkipped();

        /// Appends interleaved PCM samples and encodes every complete 10ms Opus frame.
        bool PushPcmAndEncode(const void* pcmData, size_t dataBytes, std::vector<EncodedOpusPacket>& outPackets);

    private:
        ::OpusEncoder* m_encoder = nullptr;
        uint32_t m_sampleRate = 0;
        uint16_t m_bitDepth = 0;
        uint16_t m_channels = 0;
        uint16_t m_frameSize = 0;
        uint32_t m_nextSeq = 0;
        bool m_silenceSkippedPending = false;
        std::vector<float> m_pcmFifo;
        std::vector<uint8_t> m_encodeScratch;
    };

    /// RAII 10ms streaming Opus decoder with sequence tracking, In-Band FEC, and bounded PLC.
    class OpusStreamDecoder {
    public:
        /// Constructs an uninitialized Opus decoder.
        OpusStreamDecoder();
        /// Releases underlying libopus decoder state.
        ~OpusStreamDecoder();

        OpusStreamDecoder(const OpusStreamDecoder&) = delete;
        OpusStreamDecoder& operator=(const OpusStreamDecoder&) = delete;

        /// Configures the Opus decoder for the specified output PCM stream format.
        bool Initialize(uint32_t sampleRate, uint16_t bitDepth, uint16_t channels);
        /// Destroys the libopus decoder and resets sequence tracking.
        void Shutdown();
        /// Resets decoder DSP state and optionally clears expected sequence tracking.
        void Reset(bool resetSequence = false);

        /// Returns true if the decoder is ready to decode packets.
        bool IsInitialized() const { return m_decoder != nullptr; }
        /// Returns true if the decoder matches the specified PCM format.
        bool MatchesFormat(uint32_t sampleRate, uint16_t bitDepth, uint16_t channels) const;
        /// Returns the configured sample rate in Hz.
        uint32_t GetSampleRate() const { return m_sampleRate; }
        /// Returns the configured channel count (1 or 2).
        uint16_t GetChannels() const { return m_channels; }
        /// Returns the configured output bit depth (16 or 32).
        uint16_t GetBitDepth() const { return m_bitDepth; }

        /// Decodes one incoming Opus or silence packet into target PCM format, recovering gaps via FEC/PLC.
        bool DecodePacket(uint8_t flags,
                          uint16_t frameCount,
                          uint32_t audioSeq,
                          const uint8_t* opusPayload,
                          size_t payloadBytes,
                          std::vector<uint8_t>& outPcmBytes,
                          OpusDecodeStats& outStats);

    private:
        void AppendDecodedFloatFrames(const float* samples, uint16_t frameCount, std::vector<uint8_t>& outPcmBytes) const;

        ::OpusDecoder* m_decoder = nullptr;
        uint32_t m_sampleRate = 0;
        uint16_t m_bitDepth = 0;
        uint16_t m_channels = 0;
        uint16_t m_frameSize = 0;
        uint32_t m_expectedSeq = 0;
        bool m_hasExpectedSeq = false;
        bool m_inSilence = false;
        std::vector<float> m_decodeScratch;
    };

} // namespace AudioCodec

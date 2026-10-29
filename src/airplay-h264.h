#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

constexpr int kMaxAirPlayVideoDimension = 8192;
constexpr uint64_t kMaxAirPlayPixelCount = 64ull * 1024ull * 1024ull;

// Validates video dimensions: width and height must be positive, <= 8192,
// and the total pixel count must be within safe limits (64 MP).
bool airplay_is_valid_video_geometry(int width, int height);

// Validates that an audio frame has supported sample rates (8 kHz to 192 kHz),
// valid channel counts (1 or 2), 16-bit PCM, and aligned non-zero length.
bool airplay_is_valid_audio_frame(unsigned int sample_rate,
                                 unsigned short channels,
                                 unsigned short bits_per_sample,
                                 unsigned int data_len);

// Scans an H.264 Annex B stream for SPS (NAL unit type 7).
bool airplay_h264_contains_sps(const uint8_t *data, size_t size);

// Scans an H.264 Annex B stream for PPS (NAL unit type 8).
bool airplay_h264_contains_pps(const uint8_t *data, size_t size);

// Scans an H.264 Annex B stream for IDR slices (NAL unit type 5).
bool airplay_h264_contains_idr(const uint8_t *data, size_t size);

// Returns true if the packet contains parameter sets (SPS and/or PPS) and
// contains NO video slice data (neither non-IDR slice type 1 nor IDR slice type 5).
// Such parameter-only packets must be cached rather than submitted alone to the decoder.
bool airplay_is_h264_parameter_set_only(const uint8_t *data, size_t size);

// Extracts ONLY the SPS (NAL unit 7) and PPS (NAL unit 8) parameter sets from
// a data buffer into standard 4-byte Annex B start-code formatted units.
// Slice/frame data is excluded to guarantee that cached parameters are compact (<100 bytes).
std::vector<uint8_t> airplay_extract_sps_pps(const uint8_t *data, size_t size);

// Prepares an H.264 packet for the decoder. If the packet is a keyframe or IDR slice
// and lacks an in-band SPS, cached SPS/PPS parameter sets are prepended so FFmpeg can
// initialize or refresh its sequence state without returning AVERROR_INVALIDDATA.
std::vector<uint8_t> airplay_prepare_h264_packet(
    const uint8_t *data, size_t size, int is_key, bool config_pending,
    const std::vector<uint8_t> &cached_sps_pps);


#include "airplay-h264.h"

#include <algorithm>
#include <cstring>

namespace {

struct NalScanResult {
  bool has_sps = false;
  bool has_pps = false;
  bool has_idr = false;
  bool has_slice = false;
};

NalScanResult scan_h264_nal_units(const uint8_t *data, size_t size)
{
  NalScanResult result;
  if (!data || size < 4) {
    return result;
  }

  for (size_t i = 0; i + 3 < size; ++i) {
    size_t start_code_len = 0;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      start_code_len = 3;
    } else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
               data[i + 2] == 0 && data[i + 3] == 1) {
      start_code_len = 4;
    }

    if (start_code_len > 0) {
      const size_t nal_offset = i + start_code_len;
      if (nal_offset < size) {
        const uint8_t nal_type = data[nal_offset] & 0x1F;
        if (nal_type == 7) {
          result.has_sps = true;
        } else if (nal_type == 8) {
          result.has_pps = true;
        } else if (nal_type == 5) {
          result.has_idr = true;
          result.has_slice = true;
        } else if (nal_type == 1) {
          result.has_slice = true;
        }
      }
      i += start_code_len - 1;
    }
  }

  return result;
}

} // namespace

bool airplay_is_valid_video_geometry(int width, int height)
{
  if (width <= 0 || height <= 0 || width > kMaxAirPlayVideoDimension ||
      height > kMaxAirPlayVideoDimension) {
    return false;
  }
  const uint64_t pixels = static_cast<uint64_t>(width) *
                          static_cast<uint64_t>(height);
  return pixels > 0 && pixels <= kMaxAirPlayPixelCount;
}

bool airplay_is_valid_audio_frame(unsigned int sample_rate,
                                 unsigned short channels,
                                 unsigned short bits_per_sample,
                                 unsigned int data_len)
{
  if (data_len == 0 || bits_per_sample != 16) {
    return false;
  }
  if (channels != 1 && channels != 2) {
    return false;
  }
  if (sample_rate < 8000 || sample_rate > 192000) {
    return false;
  }
  const unsigned int bytes_per_frame =
      static_cast<unsigned int>(channels) * (bits_per_sample / 8);
  if (!bytes_per_frame || (data_len % bytes_per_frame) != 0) {
    return false;
  }
  return true;
}

bool airplay_h264_contains_sps(const uint8_t *data, size_t size)
{
  return scan_h264_nal_units(data, size).has_sps;
}

bool airplay_h264_contains_pps(const uint8_t *data, size_t size)
{
  return scan_h264_nal_units(data, size).has_pps;
}

bool airplay_h264_contains_idr(const uint8_t *data, size_t size)
{
  return scan_h264_nal_units(data, size).has_idr;
}

bool airplay_is_h264_parameter_set_only(const uint8_t *data, size_t size)
{
  const NalScanResult res = scan_h264_nal_units(data, size);
  return (res.has_sps || res.has_pps) && !res.has_slice;
}

std::vector<uint8_t> airplay_extract_sps_pps(const uint8_t *data, size_t size)
{
  std::vector<uint8_t> result;
  if (!data || size < 4) {
    return result;
  }

  size_t i = 0;
  while (i + 3 < size) {
    size_t start_code_len = 0;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      start_code_len = 3;
    } else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
               data[i + 2] == 0 && data[i + 3] == 1) {
      start_code_len = 4;
    }

    if (start_code_len > 0) {
      const size_t nal_start = i + start_code_len;
      if (nal_start < size) {
        const uint8_t nal_type = data[nal_start] & 0x1F;

        // Find next start code or end of buffer
        size_t next_start = size;
        for (size_t j = nal_start; j + 3 < size; ++j) {
          if ((data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) ||
              (j + 4 <= size && data[j] == 0 && data[j + 1] == 0 &&
               data[j + 2] == 0 && data[j + 3] == 1)) {
            next_start = j;
            break;
          }
        }

        // True SPS (7) and PPS (8) NALUs are typically 4 to 64 bytes, never exceeding 128 bytes.
        if (nal_type == 7 || nal_type == 8) {
          const size_t nal_len = next_start - nal_start;
          if (nal_len > 0 && nal_len <= 128) {
            result.push_back(0);
            result.push_back(0);
            result.push_back(0);
            result.push_back(1);
            result.insert(result.end(), data + nal_start, data + next_start);
          }
        }

        i = next_start;
        continue;
      }
    }
    ++i;
  }

  // Combined SPS + PPS parameter sets never exceed 256 bytes in total
  if (result.size() > 256 || !airplay_h264_contains_sps(result.data(), result.size())) {
    result.clear();
  }

  return result;
}

std::vector<uint8_t> airplay_prepare_h264_packet(
    const uint8_t *data, size_t size, int is_key, bool config_pending,
    const std::vector<uint8_t> &cached_sps_pps)
{
  if (!data || size == 0) {
    return {};
  }

  const NalScanResult res = scan_h264_nal_units(data, size);
  const bool needs_prepend = !cached_sps_pps.empty() &&
                             (res.has_idr || is_key != 0 || config_pending) &&
                             !res.has_sps;

  if (needs_prepend) {
    std::vector<uint8_t> prepared;
    prepared.reserve(cached_sps_pps.size() + size);
    prepared.insert(prepared.end(), cached_sps_pps.begin(),
                    cached_sps_pps.end());
    prepared.insert(prepared.end(), data, data + size);
    return prepared;
  }

  return std::vector<uint8_t>(data, data + size);
}

#pragma once

#include <cstdint>
#include <string>

enum class ConnectionMode {
  Usb,
  Tcpip,
};

struct ScrcpySettings {
  std::string scrcpy_path;
  std::string serial;
  ConnectionMode connection_mode = ConnectionMode::Usb;
  std::string tcpip_address;

  std::string video_codec = "h264";
  std::string video_encoder;
  std::string video_bit_rate = "8M";
  int32_t max_size = 0;
  int32_t max_fps = 60;
  std::string video_codec_options;
  int32_t encoder_refresh_minutes = 0;

  std::string audio_codec = "opus";
  std::string audio_encoder;
  std::string audio_bit_rate = "128K";
  int32_t audio_buffer_ms = 0;
  std::string audio_source = "output";
  std::string audio_codec_options;

  bool cv_bridge_enabled = false;
  int32_t cv_bridge_fps = 5;
  int32_t cv_bridge_max_width = 0;
};

constexpr const char *kDefaultScrcpyPath =
    "C:/Program Files/scrcpy/4.1/scrcpy.exe";

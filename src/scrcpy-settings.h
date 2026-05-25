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
  int32_t max_fps = 0;
  std::string video_codec_options;

  std::string audio_codec = "opus";
  std::string audio_encoder;
  std::string audio_bit_rate = "128K";
  int32_t audio_buffer_ms = 50;
  std::string audio_source = "output";
  std::string audio_codec_options;
};

constexpr const char *kDefaultScrcpyPath =
    "C:/Users/rokas/Downloads/scrcpy-win64-v4.0/scrcpy.exe";


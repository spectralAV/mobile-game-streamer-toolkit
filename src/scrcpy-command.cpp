#include "scrcpy-command.h"

#include "scrcpy-low-latency.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace {

void append_if_value(std::vector<std::string> &args, std::string flag,
                     const std::string &value)
{
  if (!value.empty()) {
    args.push_back(std::move(flag) + "=" + value);
  }
}

void append_if_positive(std::vector<std::string> &args, std::string flag,
                        int32_t value)
{
  if (value > 0) {
    args.push_back(std::move(flag) + "=" + std::to_string(value));
  }
}

} // namespace

std::string parse_scrcpy_version_output(const std::string &output)
{
  constexpr const char *prefix = "scrcpy ";
  const size_t prefix_pos = output.find(prefix);
  if (prefix_pos == std::string::npos) {
    return {};
  }

  const size_t version_begin =
      prefix_pos + std::char_traits<char>::length(prefix);
  size_t version_end = version_begin;
  while (version_end < output.size()) {
    const unsigned char c = static_cast<unsigned char>(output[version_end]);
    if (!std::isdigit(c) && c != '.') {
      break;
    }
    ++version_end;
  }

  if (version_end == version_begin) {
    return {};
  }
  return output.substr(version_begin, version_end - version_begin);
}

bool is_supported_scrcpy_server_version(const std::string &version)
{
  // The server protocol is private and requires an exact client version.
  // 4.1 only extends the 4.0 media protocol with VP8/VP9 codec identifiers.
  return version == "4.0" || version == "4.1";
}

bool looks_like_mediaformat_options(const std::string &value)
{
  return !value.empty() && value.find(':') != std::string::npos &&
         value.find('=') != std::string::npos;
}

std::string normalize_video_codec_options(const std::string &value)
{
  if (value.empty()) {
    return {};
  }
  if (value == "Device default (no extra options)" ||
      value == "Plugin default (auto 1s keyframes)") {
    return {};
  }
  if (!looks_like_mediaformat_options(value)) {
    return {};
  }
  return value;
}

std::string normalize_audio_codec_options(const std::string &value)
{
  if (value.empty() || value == "None") {
    return {};
  }
  if (!looks_like_mediaformat_options(value)) {
    return {};
  }
  return value;
}

std::string merge_realtime_video_codec_options(const std::string &user_options)
{
  return normalize_video_codec_options(user_options);
}

std::vector<std::string> build_adb_connect_args(const ScrcpySettings &settings)
{
  if (settings.connection_mode == ConnectionMode::Tcpip &&
      !settings.tcpip_address.empty()) {
    return {"connect", settings.tcpip_address};
  }
  return {};
}

std::vector<std::string> build_adb_selector_args(const ScrcpySettings &settings)
{
  if (settings.connection_mode == ConnectionMode::Tcpip &&
      !settings.tcpip_address.empty()) {
    return {"-s", settings.tcpip_address};
  }
  if (!settings.serial.empty()) {
    return {"-s", settings.serial};
  }
  return {"-d"};
}

std::vector<std::string> build_scrcpy_probe_args(const ScrcpySettings &settings)
{
  std::vector<std::string> args;

  args.push_back(settings.scrcpy_path.empty() ? kDefaultScrcpyPath
                                              : settings.scrcpy_path);

  if (settings.connection_mode == ConnectionMode::Tcpip &&
             !settings.tcpip_address.empty()) {
    args.push_back("--tcpip=" + settings.tcpip_address);
  } else if (!settings.serial.empty()) {
    args.push_back("--serial=" + settings.serial);
  } else if (settings.connection_mode == ConnectionMode::Usb) {
    args.push_back("--select-usb");
  }

  args.push_back("--no-control");
  args.push_back("--no-window");

  append_if_value(args, "--video-codec", settings.video_codec);
  append_if_value(args, "--video-encoder", settings.video_encoder);
  append_if_value(args, "--video-bit-rate", settings.video_bit_rate);
  append_if_positive(args, "--max-size", settings.max_size);
  append_if_positive(args, "--max-fps", settings.max_fps);
  append_if_value(args, "--video-codec-options",
                  merge_realtime_video_codec_options(settings.video_codec_options));

  append_if_value(args, "--audio-codec", settings.audio_codec);
  append_if_value(args, "--audio-encoder", settings.audio_encoder);
  append_if_value(args, "--audio-bit-rate", settings.audio_bit_rate);
  append_if_positive(
      args, "--audio-buffer",
      std::min(settings.audio_buffer_ms, kMaxRealtimeDelayMs));
  append_if_value(args, "--audio-source", settings.audio_source);
  append_if_value(args, "--audio-codec-options",
                  normalize_audio_codec_options(settings.audio_codec_options));

  return args;
}

std::string quote_for_log(const std::vector<std::string> &argv)
{
  std::ostringstream out;

  for (size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      out << ' ';
    }

    const std::string &arg = argv[i];
    if (arg.find_first_of(" \t\"") == std::string::npos) {
      out << arg;
      continue;
    }

    out << '"';
    for (char c : arg) {
      if (c == '"') {
        out << "\\\"";
      } else {
        out << c;
      }
    }
    out << '"';
  }

  return out.str();
}

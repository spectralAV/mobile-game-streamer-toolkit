#include "scrcpy-command.h"

#include <sstream>

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

std::vector<std::string> build_scrcpy_probe_args(const ScrcpySettings &settings)
{
  std::vector<std::string> args;

  args.push_back(settings.scrcpy_path.empty() ? kDefaultScrcpyPath
                                              : settings.scrcpy_path);

  if (!settings.serial.empty()) {
    args.push_back("--serial=" + settings.serial);
  } else if (settings.connection_mode == ConnectionMode::Tcpip &&
             !settings.tcpip_address.empty()) {
    args.push_back("--tcpip=" + settings.tcpip_address);
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
  append_if_value(args, "--video-codec-options", settings.video_codec_options);

  append_if_value(args, "--audio-codec", settings.audio_codec);
  append_if_value(args, "--audio-encoder", settings.audio_encoder);
  append_if_value(args, "--audio-bit-rate", settings.audio_bit_rate);
  append_if_positive(args, "--audio-buffer", settings.audio_buffer_ms);
  append_if_value(args, "--audio-source", settings.audio_source);
  append_if_value(args, "--audio-codec-options", settings.audio_codec_options);

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


#include "scrcpy-source.h"

#include "scrcpy-command.h"
#include "scrcpy-low-latency.h"
#include "scrcpy-runtime.h"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <vector>

namespace {

const char *obs_data_get_string_or_default(obs_data_t *data, const char *name,
                                           const char *fallback)
{
  const char *value = obs_data_get_string(data, name);
  return value && value[0] ? value : fallback;
}

void add_codec_list(obs_properties_t *props, const char *id, const char *text,
                    const char *const *values, size_t count)
{
  obs_property_t *list =
      obs_properties_add_list(props, id, text, OBS_COMBO_TYPE_LIST,
                              OBS_COMBO_FORMAT_STRING);
  for (size_t i = 0; i < count; ++i) {
    obs_property_list_add_string(list, values[i], values[i]);
  }
}

std::string normalize_path(std::string path)
{
  for (char &c : path) {
    if (c == '/') {
      c = '\\';
    }
  }
  return path;
}

std::string scrcpy_dir_from_path(const std::string &scrcpy_path)
{
  size_t slash = scrcpy_path.find_last_of("/\\");
  if (slash == std::string::npos) {
    return ".";
  }
  return scrcpy_path.substr(0, slash);
}

std::string adb_path_from_scrcpy(const std::string &scrcpy_path)
{
  return normalize_path(scrcpy_dir_from_path(scrcpy_path) + "\\adb.exe");
}

std::string quote_cmd_arg(const std::string &arg)
{
  std::string out = "\"";
  for (char c : arg) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

struct AdbDeviceInfo {
  std::string serial;
  std::string label;
};

struct EncoderInfo {
  std::string media_type;
  std::string codec;
  std::string encoder;
  std::string detail;
};

ScrcpySettings read_settings(obs_data_t *data);

std::vector<AdbDeviceInfo> scan_adb_devices(const std::string &scrcpy_path)
{
  std::vector<AdbDeviceInfo> devices;
  const std::string adb_path = adb_path_from_scrcpy(scrcpy_path);
  const std::string command = quote_cmd_arg(adb_path) + " devices -l";

  FILE *pipe = _popen(command.c_str(), "r");
  if (!pipe) {
    return devices;
  }

  std::array<char, 1024> buffer = {};
  bool first_line = true;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
    std::string line = buffer.data();
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }

    if (first_line) {
      first_line = false;
      continue;
    }
    if (line.empty()) {
      continue;
    }

    std::istringstream stream(line);
    std::string serial;
    std::string state;
    stream >> serial >> state;
    if (serial.empty() || state != "device") {
      continue;
    }

    std::string token;
    std::string model;
    std::string transport;
    while (stream >> token) {
      if (token.rfind("model:", 0) == 0) {
        model = token.substr(6);
      } else if (token.rfind("transport_id:", 0) == 0) {
        transport = token.substr(13);
      }
    }

    std::string label = serial;
    if (!model.empty()) {
      label += " (" + model + ")";
    }
    if (!transport.empty()) {
      label += " transport " + transport;
    }

    devices.push_back({serial, label});
  }

  _pclose(pipe);
  return devices;
}

std::string trim(std::string value)
{
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }

  size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t')) {
    ++first;
  }

  return first ? value.substr(first) : value;
}

std::string value_after_flag(const std::string &line, const char *flag)
{
  size_t pos = line.find(flag);
  if (pos == std::string::npos) {
    return {};
  }

  pos += strlen(flag);
  size_t end = line.find_first_of(" \t\r\n", pos);
  return line.substr(pos, end == std::string::npos ? std::string::npos
                                                   : end - pos);
}

std::vector<EncoderInfo> scan_scrcpy_encoders(const ScrcpySettings &settings)
{
  std::vector<EncoderInfo> encoders;
  std::string command =
      quote_cmd_arg(settings.scrcpy_path.empty() ? kDefaultScrcpyPath
                                                 : settings.scrcpy_path);

  if (settings.connection_mode == ConnectionMode::Tcpip &&
      !settings.tcpip_address.empty()) {
    command += " --tcpip=" + quote_cmd_arg(settings.tcpip_address);
  } else if (!settings.serial.empty()) {
    command += " --serial=" + quote_cmd_arg(settings.serial);
  } else if (settings.connection_mode == ConnectionMode::Usb) {
    command += " --select-usb";
  }

  command += " --list-encoders 2>&1";

  FILE *pipe = _popen(command.c_str(), "r");
  if (!pipe) {
    return encoders;
  }

  std::array<char, 2048> buffer = {};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
    std::string line = trim(buffer.data());
    std::string media_type;
    std::string codec;
    std::string encoder;

    if (line.find("--video-codec=") != std::string::npos) {
      media_type = "video";
      codec = value_after_flag(line, "--video-codec=");
      encoder = value_after_flag(line, "--video-encoder=");
    } else if (line.find("--audio-codec=") != std::string::npos) {
      media_type = "audio";
      codec = value_after_flag(line, "--audio-codec=");
      encoder = value_after_flag(line, "--audio-encoder=");
    }

    if (media_type.empty() || codec.empty() || encoder.empty()) {
      continue;
    }

    std::string detail;
    size_t detail_pos = line.find(encoder);
    if (detail_pos != std::string::npos) {
      detail = trim(line.substr(detail_pos + encoder.size()));
    }

    encoders.push_back({media_type, codec, encoder, detail});
  }

  _pclose(pipe);
  return encoders;
}

void populate_device_list(obs_property_t *list, const std::string &scrcpy_path,
                          const std::string &selected)
{
  obs_property_list_clear(list);
  obs_property_list_add_string(list, "Auto USB device", "");

  bool selected_found = selected.empty();
  for (const AdbDeviceInfo &device : scan_adb_devices(scrcpy_path)) {
    obs_property_list_add_string(list, device.label.c_str(),
                                 device.serial.c_str());
    if (device.serial == selected) {
      selected_found = true;
    }
  }

  if (!selected_found) {
    std::string label = selected + " (not currently connected)";
    obs_property_list_add_string(list, label.c_str(), selected.c_str());
  }
}

void populate_encoder_list(obs_property_t *list,
                           const std::vector<EncoderInfo> &encoders,
                           const char *media_type,
                           const std::string &codec,
                           const std::string &selected)
{
  obs_property_list_clear(list);
  obs_property_list_add_string(list, "Auto", "");

  bool selected_found = selected.empty();
  for (const EncoderInfo &encoder : encoders) {
    if (encoder.media_type != media_type || encoder.codec != codec) {
      continue;
    }

    std::string label = encoder.encoder + " (" + encoder.codec;
    if (!encoder.detail.empty()) {
      label += ", " + encoder.detail;
    }
    label += ")";
    obs_property_list_add_string(list, label.c_str(), encoder.encoder.c_str());
    if (encoder.encoder == selected) {
      selected_found = true;
    }
  }

  if (!selected_found) {
    std::string label = selected + " (not currently listed)";
    obs_property_list_add_string(list, label.c_str(), selected.c_str());
  }
}

void populate_video_codec_options_list(obs_property_t *list,
                                       const std::string &selected)
{
  const std::string normalized = normalize_video_codec_options(selected);
  struct Preset {
    const char *label;
    const char *value;
  };

  static const Preset presets[] = {
      {"Device default (no extra options)", ""},
      {"1s keyframes (i-frame-interval:float=1)", "i-frame-interval:float=1"},
      {"0.5s keyframes (i-frame-interval:float=0.5)",
       "i-frame-interval:float=0.5"},
      {"2s keyframes (i-frame-interval:float=2)", "i-frame-interval:float=2"},
  };

  obs_property_list_clear(list);
  bool selected_found = normalized.empty();
  for (const Preset &preset : presets) {
    obs_property_list_add_string(list, preset.label, preset.value);
    if (normalized == preset.value) {
      selected_found = true;
    }
  }

  if (!selected_found && !normalized.empty()) {
    const std::string label = normalized + " (custom)";
    obs_property_list_add_string(list, label.c_str(), normalized.c_str());
    selected_found = true;
  }

  if (!selected_found && !selected.empty()) {
    obs_property_list_add_string(list, selected.c_str(), normalized.c_str());
  }
}

void populate_audio_codec_options_list(obs_property_t *list,
                                       const std::string &selected)
{
  const std::string normalized = normalize_audio_codec_options(selected);
  obs_property_list_clear(list);
  obs_property_list_add_string(list, "None", "");

  if (!normalized.empty()) {
    obs_property_list_add_string(list, normalized.c_str(), normalized.c_str());
  } else if (!selected.empty() && selected != "None") {
    obs_property_list_add_string(list, selected.c_str(), "");
  }
}

ScrcpySettings settings_from_props(obs_data_t *data)
{
  return read_settings(data);
}

void populate_encoder_lists(obs_properties_t *props,
                            const ScrcpySettings &settings)
{
  const std::vector<EncoderInfo> encoders = scan_scrcpy_encoders(settings);
  obs_property_t *video_encoder = obs_properties_get(props, "video_encoder");
  if (video_encoder) {
    populate_encoder_list(video_encoder, encoders, "video",
                          settings.video_codec, settings.video_encoder);
  }

  obs_property_t *audio_encoder = obs_properties_get(props, "audio_encoder");
  if (audio_encoder) {
    populate_encoder_list(audio_encoder, encoders, "audio",
                          settings.audio_codec, settings.audio_encoder);
  }
}

bool refresh_devices_clicked(obs_properties_t *props, obs_property_t *,
                             void *data)
{
  auto *source = static_cast<ScrcpySource *>(data);
  ScrcpySettings settings = source ? source->settings_snapshot()
                                   : ScrcpySettings{};
  if (settings.scrcpy_path.empty()) {
    settings.scrcpy_path = kDefaultScrcpyPath;
  }

  obs_property_t *serial = obs_properties_get(props, "serial");
  if (serial) {
    populate_device_list(serial, settings.scrcpy_path, settings.serial);
  }
  populate_encoder_lists(props, settings);

  return true;
}

bool refresh_encoders_clicked(obs_properties_t *props, obs_property_t *,
                              void *data)
{
  auto *source = static_cast<ScrcpySource *>(data);
  ScrcpySettings settings = source ? source->settings_snapshot()
                                   : ScrcpySettings{};
  if (settings.scrcpy_path.empty()) {
    settings.scrcpy_path = kDefaultScrcpyPath;
  }

  populate_encoder_lists(props, settings);
  return true;
}

bool restart_stream_clicked(obs_properties_t *, obs_property_t *, void *data)
{
  auto *source = static_cast<ScrcpySource *>(data);
  if (source) {
    source->request_restart();
  }
  return true;
}

bool scrcpy_path_modified(obs_properties_t *props, obs_property_t *,
                          obs_data_t *settings)
{
  const char *path =
      obs_data_get_string_or_default(settings, "scrcpy_path",
                                     kDefaultScrcpyPath);
  const char *selected = obs_data_get_string_or_default(settings, "serial", "");
  obs_property_t *serial = obs_properties_get(props, "serial");
  if (serial) {
    populate_device_list(serial, path, selected);
  }
  return true;
}

ScrcpySettings read_settings(obs_data_t *data)
{
  ScrcpySettings settings;
  settings.scrcpy_path =
      obs_data_get_string_or_default(data, "scrcpy_path", kDefaultScrcpyPath);
  settings.serial = obs_data_get_string_or_default(data, "serial", "");

  const char *connection =
      obs_data_get_string_or_default(data, "connection_mode", "usb");
  settings.connection_mode =
      strcmp(connection, "tcpip") == 0 ? ConnectionMode::Tcpip
                                       : ConnectionMode::Usb;
  settings.tcpip_address =
      obs_data_get_string_or_default(data, "tcpip_address", "");

  settings.video_codec =
      obs_data_get_string_or_default(data, "video_codec", "h264");
  settings.video_encoder =
      obs_data_get_string_or_default(data, "video_encoder", "");
  settings.video_bit_rate =
      obs_data_get_string_or_default(data, "video_bit_rate", "8M");
  settings.max_size = static_cast<int32_t>(obs_data_get_int(data, "max_size"));
  if (settings.max_size > 0 && settings.max_size < 64) {
    blog(LOG_WARNING,
         "[obs-scrcpy-source] ignoring invalid max size %d; use 0 for automatic or at least 64",
         settings.max_size);
    settings.max_size = 0;
  }
  settings.max_fps = static_cast<int32_t>(obs_data_get_int(data, "max_fps"));
  settings.video_codec_options = normalize_video_codec_options(
      obs_data_get_string_or_default(data, "video_codec_options", ""));
  settings.encoder_refresh_minutes = std::clamp(
      static_cast<int32_t>(obs_data_get_int(data, "encoder_refresh_minutes")),
      0, 60);

  settings.audio_codec =
      obs_data_get_string_or_default(data, "audio_codec", "opus");
  settings.audio_encoder =
      obs_data_get_string_or_default(data, "audio_encoder", "");
  settings.audio_bit_rate =
      obs_data_get_string_or_default(data, "audio_bit_rate", "128K");
  settings.audio_buffer_ms = std::clamp(
      static_cast<int32_t>(obs_data_get_int(data, "audio_buffer_ms")), 0,
      kMaxRealtimeDelayMs);
  settings.audio_source =
      obs_data_get_string_or_default(data, "audio_source", "output");
  settings.audio_codec_options = normalize_audio_codec_options(
      obs_data_get_string_or_default(data, "audio_codec_options", ""));
  settings.cv_bridge_enabled = obs_data_get_bool(data, "cv_bridge_enabled");
  settings.cv_bridge_fps =
      std::clamp(static_cast<int32_t>(obs_data_get_int(data, "cv_bridge_fps")),
                 1, 10);
  settings.cv_bridge_max_width =
      std::clamp(static_cast<int32_t>(
                     obs_data_get_int(data, "cv_bridge_max_width")),
                 0, 4096);

  return settings;
}

} // namespace

ScrcpySource::ScrcpySource(obs_source_t *source)
    : source_(source), runtime_(std::make_unique<ScrcpyRuntime>(source))
{
  obs_source_set_async_unbuffered(source_, true);
}

ScrcpySource::~ScrcpySource()
{
  stop();
}

void ScrcpySource::update(obs_data_t *settings)
{
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ScrcpySettings next_settings = read_settings(settings);
    std::string next_command =
        quote_for_log(build_scrcpy_probe_args(next_settings));
    next_command += next_settings.cv_bridge_enabled
                        ? " cv_bridge=on cv_bridge_fps=" +
                              std::to_string(next_settings.cv_bridge_fps) +
                              " cv_bridge_width=" +
                              std::to_string(next_settings.cv_bridge_max_width)
                        : " cv_bridge=off";
    changed = next_command != last_command_;
    settings_ = next_settings;
    last_command_ = next_command;
  }

  if (changed) {
    restart();
  }
}

ScrcpySettings ScrcpySource::settings_snapshot() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_;
}

uint32_t ScrcpySource::width() const
{
  return runtime_ ? runtime_->width() : width_;
}

uint32_t ScrcpySource::height() const
{
  return runtime_ ? runtime_->height() : height_;
}

void ScrcpySource::tick(float)
{}

void ScrcpySource::request_restart()
{
  restart();
}

void ScrcpySource::restart()
{
  ScrcpySettings settings;
  std::string command;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    settings = settings_;
    command = last_command_;
  }

  blog(LOG_INFO, "[obs-scrcpy-source] starting: %s", command.c_str());
  runtime_->start(settings);
}

void ScrcpySource::stop()
{
  if (runtime_) {
    runtime_->stop();
  }
}

void scrcpy_source_get_defaults(obs_data_t *settings)
{
  obs_data_set_default_string(settings, "scrcpy_path", kDefaultScrcpyPath);
  obs_data_set_default_string(settings, "connection_mode", "usb");
  obs_data_set_default_string(settings, "video_codec", "h264");
  obs_data_set_default_string(settings, "video_bit_rate", "8M");
  obs_data_set_default_int(settings, "max_size", 0);
  obs_data_set_default_int(settings, "max_fps", 60);
  obs_data_set_default_int(settings, "encoder_refresh_minutes", 0);
  obs_data_set_default_string(settings, "audio_codec", "opus");
  obs_data_set_default_string(settings, "audio_bit_rate", "128K");
  obs_data_set_default_int(settings, "audio_buffer_ms", 0);
  obs_data_set_default_string(settings, "audio_source", "output");
  obs_data_set_default_bool(settings, "cv_bridge_enabled", false);
  obs_data_set_default_int(settings, "cv_bridge_fps", 5);
  obs_data_set_default_int(settings, "cv_bridge_max_width", 0);
}

obs_properties_t *scrcpy_source_get_properties(void *data)
{
  obs_properties_t *props = obs_properties_create();

  auto *source = static_cast<ScrcpySource *>(data);
  ScrcpySettings settings =
      source ? source->settings_snapshot() : ScrcpySettings{};
  if (settings.scrcpy_path.empty()) {
    settings.scrcpy_path = kDefaultScrcpyPath;
  }

  obs_property_t *path = obs_properties_add_path(
      props, "scrcpy_path", obs_module_text("ScrcpySource.ScrcpyPath"),
      OBS_PATH_FILE, "scrcpy.exe (*.exe)", nullptr);
  obs_property_set_modified_callback(path, scrcpy_path_modified);

  obs_property_t *serial =
      obs_properties_add_list(props, "serial",
                              obs_module_text("ScrcpySource.Serial"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  populate_device_list(serial, settings.scrcpy_path, settings.serial);

  obs_properties_add_button2(props, "refresh_devices",
                             obs_module_text("ScrcpySource.RefreshDevices"),
                             refresh_devices_clicked, data);
  obs_properties_add_button2(props, "refresh_encoders",
                             obs_module_text("ScrcpySource.RefreshEncoders"),
                             refresh_encoders_clicked, data);
  obs_properties_add_button2(props, "restart_stream",
                             obs_module_text("ScrcpySource.RestartStream"),
                             restart_stream_clicked, data);

  obs_property_t *connection =
      obs_properties_add_list(props, "connection_mode",
                              obs_module_text("ScrcpySource.ConnectionMode"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(
      connection, obs_module_text("ScrcpySource.ConnectionMode.USB"), "usb");
  obs_property_list_add_string(
      connection, obs_module_text("ScrcpySource.ConnectionMode.TCPIP"),
      "tcpip");
  obs_properties_add_text(props, "tcpip_address",
                          obs_module_text("ScrcpySource.TcpipAddress"),
                          OBS_TEXT_DEFAULT);

  const char *video_codecs[] = {"h264", "h265", "av1"};
  add_codec_list(props, "video_codec",
                 obs_module_text("ScrcpySource.VideoCodec"), video_codecs,
                 3);
  obs_property_t *video_encoder =
      obs_properties_add_list(props, "video_encoder",
                              obs_module_text("ScrcpySource.VideoEncoder"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_properties_add_text(
      props, "video_encoder_help",
      obs_module_text("ScrcpySource.VideoEncoderHelp"), OBS_TEXT_INFO);
  obs_properties_add_text(props, "video_bit_rate",
                          obs_module_text("ScrcpySource.VideoBitRate"),
                          OBS_TEXT_DEFAULT);
  obs_properties_add_int(props, "max_size",
                         obs_module_text("ScrcpySource.MaxSize"), 0, 8192, 16);
  obs_properties_add_int(props, "max_fps",
                         obs_module_text("ScrcpySource.MaxFps"), 0, 240, 1);
  obs_property_t *video_codec_options = obs_properties_add_list(
      props, "video_codec_options",
      obs_module_text("ScrcpySource.VideoCodecOptions"), OBS_COMBO_TYPE_EDITABLE,
      OBS_COMBO_FORMAT_STRING);
  obs_properties_add_text(
      props, "video_codec_options_help",
      obs_module_text("ScrcpySource.VideoCodecOptionsHelp"), OBS_TEXT_INFO);
  populate_video_codec_options_list(video_codec_options,
                                    settings.video_codec_options);
  obs_properties_add_int(
      props, "encoder_refresh_minutes",
      obs_module_text("ScrcpySource.EncoderRefreshMinutes"), 0, 60, 1);

  const char *audio_codecs[] = {"opus", "aac", "flac", "raw"};
  add_codec_list(props, "audio_codec",
                 obs_module_text("ScrcpySource.AudioCodec"), audio_codecs,
                 4);
  obs_property_t *audio_encoder =
      obs_properties_add_list(props, "audio_encoder",
                              obs_module_text("ScrcpySource.AudioEncoder"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_properties_add_text(
      props, "audio_encoder_help",
      obs_module_text("ScrcpySource.AudioEncoderHelp"), OBS_TEXT_INFO);
  obs_properties_add_text(props, "audio_bit_rate",
                          obs_module_text("ScrcpySource.AudioBitRate"),
                          OBS_TEXT_DEFAULT);
  obs_properties_add_int(props, "audio_buffer_ms",
                         obs_module_text("ScrcpySource.AudioBufferMs"), 0,
                         kMaxRealtimeDelayMs, 5);

  const char *audio_sources[] = {"output",
                                 "playback",
                                 "mic",
                                 "mic-unprocessed",
                                 "mic-camcorder",
                                 "mic-voice-recognition",
                                 "mic-voice-communication"};
  add_codec_list(props, "audio_source",
                 obs_module_text("ScrcpySource.AudioSource"), audio_sources,
                 7);
  obs_property_t *audio_codec_options = obs_properties_add_list(
      props, "audio_codec_options",
      obs_module_text("ScrcpySource.AudioCodecOptions"), OBS_COMBO_TYPE_EDITABLE,
      OBS_COMBO_FORMAT_STRING);
  obs_properties_add_text(
      props, "audio_codec_options_help",
      obs_module_text("ScrcpySource.AudioCodecOptionsHelp"), OBS_TEXT_INFO);
  populate_audio_codec_options_list(audio_codec_options,
                                    settings.audio_codec_options);

  obs_properties_add_bool(props, "cv_bridge_enabled",
                          obs_module_text("ScrcpySource.CvBridgeEnabled"));
  obs_properties_add_int(props, "cv_bridge_fps",
                         obs_module_text("ScrcpySource.CvBridgeFps"), 1, 10,
                         1);
  obs_properties_add_int(props, "cv_bridge_max_width",
                         obs_module_text("ScrcpySource.CvBridgeMaxWidth"),
                         0, 4096, 16);

  populate_encoder_list(video_encoder, {}, "video", settings.video_codec,
                        settings.video_encoder);
  populate_encoder_list(audio_encoder, {}, "audio", settings.audio_codec,
                        settings.audio_encoder);

  return props;
}

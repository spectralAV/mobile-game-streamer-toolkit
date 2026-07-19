#include "airplay-source.h"

#include <obs-module.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {

const char *obs_data_get_string_or_default(obs_data_t *data, const char *name,
                                           const char *fallback)
{
  const char *value = obs_data_get_string(data, name);
  return value && value[0] ? value : fallback;
}

int32_t read_port(obs_data_t *data, const char *name, int32_t fallback)
{
  const int32_t value = static_cast<int32_t>(obs_data_get_int(data, name));
  return value >= 1 && value <= 65535 ? value : fallback;
}

std::string trim_string(std::string value)
{
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\r' || value.back() == '\n')) {
    value.pop_back();
  }
  size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t')) {
    ++first;
  }
  return first ? value.substr(first) : value;
}

AirPlaySettings read_settings(obs_data_t *data)
{
  AirPlaySettings settings;
  settings.enabled = obs_data_get_bool(data, "airplay_enabled");
  settings.receiver_name = trim_string(obs_data_get_string_or_default(
      data, "airplay_receiver_name", kDefaultAirPlayReceiverName));
  if (settings.receiver_name.empty()) {
    settings.receiver_name = kDefaultAirPlayReceiverName;
  }
  settings.dll_path =
      obs_data_get_string_or_default(data, "airplay_dll_path", "");
  settings.raop_port =
      read_port(data, "airplay_raop_port", kDefaultAirPlayRaopPort);
  settings.airplay_port =
      read_port(data, "airplay_port", kDefaultAirPlayPort);
  return settings;
}

std::string settings_signature(const AirPlaySettings &settings)
{
  std::ostringstream out;
  out << (settings.enabled ? "1" : "0") << '|'
      << settings.receiver_name << '|'
      << settings.dll_path << '|'
      << settings.raop_port << '|'
      << settings.airplay_port;
  return out.str();
}

bool restart_clicked(obs_properties_t *, obs_property_t *, void *data)
{
  auto *source = static_cast<AirPlaySource *>(data);
  if (source) {
    source->request_restart();
  }
  return true;
}

} // namespace

AirPlaySource::AirPlaySource(obs_source_t *source)
    : source_(source), runtime_(std::make_unique<AirPlayRuntime>(source))
{
  obs_source_set_async_unbuffered(source_, true);
}

AirPlaySource::~AirPlaySource()
{
  stop();
}

void AirPlaySource::update(obs_data_t *settings)
{
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    AirPlaySettings next_settings = read_settings(settings);
    const std::string next_signature = settings_signature(next_settings);
    changed = next_signature != last_signature_;
    settings_ = next_settings;
    last_signature_ = next_signature;
  }

  if (changed) {
    restart();
  }
}

AirPlaySettings AirPlaySource::settings_snapshot() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_;
}

uint32_t AirPlaySource::width() const
{
  return runtime_ ? runtime_->width() : 1280;
}

uint32_t AirPlaySource::height() const
{
  return runtime_ ? runtime_->height() : 720;
}

void AirPlaySource::request_restart()
{
  restart();
}

void AirPlaySource::restart()
{
  AirPlaySettings settings;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    settings = settings_;
  }

  blog(LOG_INFO,
       "[obs-airplay-source] starting receiver '%s' on ports %d/%d",
       settings.receiver_name.c_str(), settings.raop_port,
       settings.airplay_port);
  if (!settings.enabled) {
    blog(LOG_INFO, "[obs-airplay-source] receiver disabled; not starting");
    stop();
    return;
  }
  runtime_->start(settings);
}

void AirPlaySource::stop()
{
  if (runtime_) {
    runtime_->stop();
  }
}

void airplay_source_get_defaults(obs_data_t *settings)
{
  obs_data_set_default_bool(settings, "airplay_enabled", false);
  obs_data_set_default_string(settings, "airplay_receiver_name",
                              kDefaultAirPlayReceiverName);
  obs_data_set_default_string(settings, "airplay_dll_path", "");
  obs_data_set_default_int(settings, "airplay_raop_port",
                           kDefaultAirPlayRaopPort);
  obs_data_set_default_int(settings, "airplay_port", kDefaultAirPlayPort);
}

obs_properties_t *airplay_source_get_properties(void *data)
{
  obs_properties_t *props = obs_properties_create();

  obs_properties_add_bool(props, "airplay_enabled",
                          obs_module_text("AirPlaySource.Enabled"));
  obs_properties_add_text(props, "airplay_receiver_name",
                          obs_module_text("AirPlaySource.ReceiverName"),
                          OBS_TEXT_DEFAULT);
  obs_properties_add_path(props, "airplay_dll_path",
                          obs_module_text("AirPlaySource.DllPath"),
                          OBS_PATH_FILE, "airplay2dll.dll (*.dll)", nullptr);
  obs_properties_add_int(props, "airplay_raop_port",
                         obs_module_text("AirPlaySource.RaopPort"), 1, 65535,
                         1);
  obs_properties_add_int(props, "airplay_port",
                         obs_module_text("AirPlaySource.AirPlayPort"), 1,
                         65535, 1);
  obs_properties_add_button2(props, "airplay_restart",
                             obs_module_text("AirPlaySource.RestartReceiver"),
                             restart_clicked, data);
  obs_properties_add_text(props, "airplay_help",
                          obs_module_text("AirPlaySource.Help"),
                          OBS_TEXT_INFO);

  return props;
}

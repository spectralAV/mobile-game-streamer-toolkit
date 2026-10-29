#include "airplay-source.h"
#include "iphone-tether-client.h"

#include <obs-module.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <thread>

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
  settings.usb_tether_enabled = obs_data_get_bool(data, "airplay_usb_tether_enabled");
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

  settings.resolution = obs_data_get_string_or_default(
      data, "airplay_resolution", "1440p");
  if (settings.resolution == "4k" || settings.resolution == "2160p") {
    settings.resolution = "4k";
    settings.display_width = 3840;
    settings.display_height = 2160;
  } else if (settings.resolution == "portrait_2k") {
    settings.resolution = "portrait_2k";
    settings.display_width = 1440;
    settings.display_height = 2560;
  } else if (settings.resolution == "portrait_4k") {
    settings.resolution = "portrait_4k";
    settings.display_width = 2160;
    settings.display_height = 3840;
  } else if (settings.resolution == "1080p") {
    settings.resolution = "1080p";
    settings.display_width = 1920;
    settings.display_height = 1080;
  } else {
    settings.resolution = "1440p";
    settings.display_width = 2560;
    settings.display_height = 1440;
  }

  if (obs_data_has_user_value(data, "show_disconnected_frame")) {
    settings.show_disconnected_frame =
        obs_data_get_bool(data, "show_disconnected_frame");
  } else if (obs_data_has_default_value(data, "show_disconnected_frame")) {
    settings.show_disconnected_frame =
        obs_data_get_bool(data, "show_disconnected_frame");
  } else {
    settings.show_disconnected_frame = true;
  }

  return settings;
}

std::string settings_signature(const AirPlaySettings &settings)
{
  std::ostringstream out;
  out << (settings.enabled ? "1" : "0") << '|'
      << settings.receiver_name << '|'
      << settings.dll_path << '|'
      << settings.raop_port << '|'
      << settings.airplay_port << '|'
      << settings.resolution << '|'
      << (settings.usb_tether_enabled ? "1" : "0") << '|'
      << (settings.show_disconnected_frame ? "1" : "0");
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

bool tether_toggle_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
  const bool enabled = obs_data_get_bool(settings, "airplay_usb_tether_enabled");
  obs_property_t *p_status = obs_properties_get(props, "airplay_usb_tether_status");
  if (p_status) {
    if (enabled) {
      obs_property_set_description(p_status, "USB Status: Activating USB Reverse Tethering... (click Refresh in 3s)");
    } else {
      obs_property_set_description(p_status, "USB Status: Disconnecting (reverting Windows defaults)...");
    }
  }
  return true;
}

bool tether_refresh_clicked(obs_properties_t *props, obs_property_t *, void *)
{
  const IPhoneTetherStatus st = IPhoneTetherClient::Instance().GetStatus();
  obs_property_t *p_status = obs_properties_get(props, "airplay_usb_tether_status");
  if (p_status) {
    std::string status_line = st.state_text.empty() ? "Disconnected (Cable idle)" : st.state_text;
    obs_property_set_description(p_status, ("USB Status: " + status_line).c_str());
  }
  return true;
}

bool tether_restart_clicked(obs_properties_t *props, obs_property_t *, void *)
{
  std::string err;
  IPhoneTetherClient::Instance().StopTethering(false, err);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  IPhoneTetherClient::Instance().StartTethering(err);
  const IPhoneTetherStatus st = IPhoneTetherClient::Instance().GetStatus();
  obs_property_t *p_status = obs_properties_get(props, "airplay_usb_tether_status");
  if (p_status) {
    std::string status_line = st.state_text.empty() ? "Re-detecting iPhone..." : st.state_text;
    obs_property_set_description(p_status, ("USB Status: " + status_line).c_str());
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
  if (IPhoneTetherClient::Instance().IsActive()) {
    std::string err;
    IPhoneTetherClient::Instance().StopTethering(false, err);
  }
}

void AirPlaySource::update(obs_data_t *settings)
{
  bool changed = false;
  bool tether_changed = false;
  bool tether_enabled = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    AirPlaySettings next_settings = read_settings(settings);
    tether_changed = next_settings.usb_tether_enabled != settings_.usb_tether_enabled;
    tether_enabled = next_settings.usb_tether_enabled;
    const std::string next_signature = settings_signature(next_settings);
    changed = next_signature != last_signature_;
    settings_ = next_settings;
    last_signature_ = next_signature;
  }

  if (tether_changed) {
    if (tether_enabled) {
      blog(LOG_INFO, "[obs-airplay-source] activating USB reverse tethering...");
      std::thread([]() {
        std::string err;
        if (!IPhoneTetherClient::Instance().StartTethering(err)) {
          blog(LOG_WARNING, "[obs-airplay-source] USB reverse tethering failed: %s", err.c_str());
        }
      }).detach();
    } else {
      blog(LOG_INFO, "[obs-airplay-source] stopping USB reverse tethering and reverting Windows defaults...");
      std::thread([]() {
        std::string err;
        IPhoneTetherClient::Instance().StopTethering(false, err);
      }).detach();
    }
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
  const uint32_t w = runtime_ ? runtime_->width() : 0;
  return w > 0 ? w : 1920;
}

uint32_t AirPlaySource::height() const
{
  const uint32_t h = runtime_ ? runtime_->height() : 0;
  return h > 0 ? h : 1080;
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
  obs_data_set_default_bool(settings, "airplay_usb_tether_enabled", false);
  obs_data_set_default_string(settings, "airplay_receiver_name",
                              kDefaultAirPlayReceiverName);
  obs_data_set_default_string(settings, "airplay_dll_path", "");
  obs_data_set_default_int(settings, "airplay_raop_port",
                           kDefaultAirPlayRaopPort);
  obs_data_set_default_int(settings, "airplay_port", kDefaultAirPlayPort);
  obs_data_set_default_string(settings, "airplay_resolution", "1440p");
  obs_data_set_default_bool(settings, "show_disconnected_frame", true);
}

obs_properties_t *airplay_source_get_properties(void *data)
{
  obs_properties_t *props = obs_properties_create();

  obs_properties_add_bool(props, "airplay_enabled",
                          obs_module_text("AirPlaySource.Enabled"));
  obs_property_t *p_tether = obs_properties_add_bool(
      props, "airplay_usb_tether_enabled",
      obs_module_text("AirPlaySource.UsbTetherEnabled"));
  obs_property_set_modified_callback(p_tether, tether_toggle_modified);

  const IPhoneTetherStatus st = IPhoneTetherClient::Instance().GetStatus();
  std::string status_line = st.state_text.empty() ? "Disconnected (Cable idle)" : st.state_text;
  obs_properties_add_text(
      props, "airplay_usb_tether_status",
      ("USB Status: " + status_line).c_str(), OBS_TEXT_INFO);

  obs_properties_add_button2(props, "airplay_usb_tether_refresh",
                             "Refresh USB Status",
                             tether_refresh_clicked, data);
  obs_properties_add_button2(props, "airplay_usb_tether_restart",
                             "Re-detect / Troubleshoot iPhone",
                             tether_restart_clicked, data);

  obs_properties_add_text(props, "airplay_receiver_name",
                          obs_module_text("AirPlaySource.ReceiverName"),
                          OBS_TEXT_DEFAULT);

  obs_property_t *res_list = obs_properties_add_list(
      props, "airplay_resolution",
      obs_module_text("AirPlaySource.Resolution"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(res_list, "1440p 2K (2560x1440) - Recommended", "1440p");
  obs_property_list_add_string(res_list, "4K UHD (3840x2160) - Ultra Sharp", "4k");
  obs_property_list_add_string(res_list, "Portrait 2K (1440x2560) - Crisp Vertical Mobile", "portrait_2k");
  obs_property_list_add_string(res_list, "Portrait 4K (2160x3840) - Ultra Crisp Vertical", "portrait_4k");
  obs_property_list_add_string(res_list, "1080p Full HD (1920x1080) - Standard", "1080p");

  obs_properties_add_bool(
      props, "show_disconnected_frame",
      obs_module_text("AirPlaySource.ShowDisconnectedFrame"));

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

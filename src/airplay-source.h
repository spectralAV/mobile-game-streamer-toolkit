#pragma once

#include "airplay-runtime.h"

#include <obs.h>

#include <memory>
#include <mutex>
#include <string>

class AirPlaySource {
public:
  explicit AirPlaySource(obs_source_t *source);
  ~AirPlaySource();

  void update(obs_data_t *settings);
  AirPlaySettings settings_snapshot() const;
  uint32_t width() const;
  uint32_t height() const;
  void request_restart();

private:
  void restart();
  void stop();

  obs_source_t *source_ = nullptr;
  mutable std::mutex mutex_;
  AirPlaySettings settings_;
  std::unique_ptr<AirPlayRuntime> runtime_;
  std::string last_signature_;
};

void airplay_source_get_defaults(obs_data_t *settings);
obs_properties_t *airplay_source_get_properties(void *data);

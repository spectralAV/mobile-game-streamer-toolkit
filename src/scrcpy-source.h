#pragma once

#include "scrcpy-settings.h"

#include <obs.h>

#include <memory>
#include <mutex>
#include <string>

class ScrcpyRuntime;

class ScrcpySource {
public:
  explicit ScrcpySource(obs_source_t *source);
  ~ScrcpySource();

  void update(obs_data_t *settings);
  ScrcpySettings settings_snapshot() const;
  uint32_t width() const;
  uint32_t height() const;
  void tick(float seconds);
  void request_restart();

private:
  void restart();
  void stop();

  obs_source_t *source_ = nullptr;
  mutable std::mutex mutex_;
  ScrcpySettings settings_;
  std::unique_ptr<ScrcpyRuntime> runtime_;
  std::string last_command_;
  uint32_t width_ = 1280;
  uint32_t height_ = 720;
};

void scrcpy_source_get_defaults(obs_data_t *settings);
obs_properties_t *scrcpy_source_get_properties(void *data);

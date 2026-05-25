#pragma once

#include "scrcpy-settings.h"

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

class ScrcpyRuntime {
public:
  explicit ScrcpyRuntime(obs_source_t *source);
  ~ScrcpyRuntime();

  ScrcpyRuntime(const ScrcpyRuntime &) = delete;
  ScrcpyRuntime &operator=(const ScrcpyRuntime &) = delete;

  void start(const ScrcpySettings &settings);
  void stop();

  uint32_t width() const;
  uint32_t height() const;

private:
  void run();
  void cleanup();
  bool prepare_server_socket();
  bool run_adb_checked(const std::vector<std::string> &args,
                       const char *label);
  bool start_adb_server_process();
  bool accept_streams();
  void video_loop();
  void audio_loop();

  obs_source_t *source_ = nullptr;
  ScrcpySettings settings_;

  std::atomic<bool> stopping_{false};
  std::thread worker_;
  std::thread video_thread_;
  std::thread audio_thread_;

  uintptr_t server_process_ = 0;
  uintptr_t listen_socket_ = UINTPTR_MAX;
  uintptr_t video_socket_ = UINTPTR_MAX;
  uintptr_t audio_socket_ = UINTPTR_MAX;

  uint16_t local_port_ = 0;
  uint32_t scid_ = 0;
  std::string socket_name_;
  std::string device_name_;

  std::atomic<uint32_t> width_{1280};
  std::atomic<uint32_t> height_{720};
};

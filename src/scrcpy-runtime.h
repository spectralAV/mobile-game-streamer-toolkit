#pragma once

#include "scrcpy-settings.h"
#include "scrcpy-low-latency.h"

#include <obs.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFrame;

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
  bool start_session();
  void stop_session();
  bool wait_before_retry(std::chrono::milliseconds delay);
  void cleanup();
  bool prepare_server_socket();
  bool run_adb_checked(const std::vector<std::string> &args,
                       const char *label);
  bool start_adb_server_process();
  bool accept_streams();
  void video_loop();
  void audio_loop();
  void queue_bridge_frame(const AVFrame *frame);
  void bridge_loop();

  obs_source_t *source_ = nullptr;
  ScrcpySettings settings_;

  std::atomic<bool> stopping_{false};
  std::atomic<bool> video_stream_ended_{false};
  std::thread worker_;
  std::thread video_thread_;
  std::thread audio_thread_;
  std::thread bridge_thread_;

  std::mutex bridge_mutex_;
  std::condition_variable bridge_condition_;
  AVFrame *bridge_pending_frame_ = nullptr;
  std::atomic<bool> bridge_busy_{false};
  std::chrono::steady_clock::time_point last_bridge_frame_at_{};

  uintptr_t server_process_ = 0;
  uintptr_t listen_socket_ = UINTPTR_MAX;
  uintptr_t video_socket_ = UINTPTR_MAX;
  uintptr_t audio_socket_ = UINTPTR_MAX;

  uint16_t local_port_ = 0;
  uint32_t scid_ = 0;
  std::string socket_name_;
  std::string device_name_;
  std::string server_version_;

  std::atomic<uint32_t> width_{1280};
  std::atomic<uint32_t> height_{720};

  std::atomic<uint64_t> latest_video_pts_us_{0};
  std::atomic<uint64_t> latest_presented_video_pts_us_{0};
  std::atomic<uint64_t> first_video_frame_at_ns_{0};
};

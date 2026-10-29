#pragma once

#include "iphone-tether.h"
#include <memory>
#include <mutex>
#include <string>
#include <atomic>

class IPhoneTetherClient {
public:
  static IPhoneTetherClient &Instance();

  IPhoneTetherClient();
  ~IPhoneTetherClient();

  IPhoneTetherClient(const IPhoneTetherClient &) = delete;
  IPhoneTetherClient &operator=(const IPhoneTetherClient &) = delete;

  // Requests reverse tethering to start asynchronously or synchronously
  bool StartTethering(std::string &error_msg);

  // Requests reverse tethering to stop and restore Windows defaults
  bool StopTethering(bool full_revert, std::string &error_msg);

  // Queries status (cached or live from helper/in-process)
  IPhoneTetherStatus GetStatus();

  bool IsActive() const { return is_active_; }

  // Called when OBS exits or plugin unloads
  void OnObsShutdown();

private:
  bool EnsureConnected(std::string &error_msg);
  bool SendPipeCommand(const std::string &cmd, std::string &response, std::string &error_msg);
  bool LaunchHelperElevated();

  std::mutex mutex_;
  std::atomic<bool> is_active_{false};
  void *pipe_handle_ = nullptr;
  std::unique_ptr<IPhoneTetherManager> in_process_manager_;
  bool use_in_process_ = false;
  IPhoneTetherStatus last_status_;
};

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>

struct IPhoneTetherStatus {
  bool active = false;
  bool connecting = false;
  std::string state_text;
  std::string phone_name;
  std::string phone_id;
  std::string adapter_name;
  std::string uplink_name;
  std::string leased_ip;
  uint64_t rx_bytes = 0;
  uint64_t tx_bytes = 0;
  uint32_t rx_speed_kbps = 0;
  uint32_t tx_speed_kbps = 0;
  std::string error_message;
};

enum class TetherMonitorState {
  Idle,
  Disconnected,
  WaitingTrust,
  SwitchingMode,
  ConfiguringIcs,
  WaitingDhcp,
  Active,
  Error
};

class IPhoneTetherManager {
public:
  IPhoneTetherManager();
  ~IPhoneTetherManager();

  IPhoneTetherManager(const IPhoneTetherManager &) = delete;
  IPhoneTetherManager &operator=(const IPhoneTetherManager &) = delete;

  // Starts reverse tethering (mode switch, CDC-NCM enumeration, ICS sharing)
  bool StartShare(std::string &error_msg);

  // Stops reverse tethering (disables ICS, restores registry defaults, re-enables PTP)
  bool StopShare(bool full_revert, std::string &error_msg);

  // Queries current live status, link rates, and DHCP lease
  IPhoneTetherStatus GetStatus();

  bool IsRunning() const { return running_; }
  bool WasActive() const { return was_active_; }
  bool NeedsCleanup() const
  {
    return monitor_running_.load() || running_ || was_active_ || filter_attached_.load();
  }

private:
  void MonitorLoop();
  bool EnsureLibusbLoaded();
  bool FindIPhoneDevice(std::wstring &instance_id, std::wstring &friendly_name);
  bool EnsureDeviceHealthy(const std::wstring &phone_id);
  bool FindAllPtpInterfaces(std::vector<std::wstring> &ptp_ids);
  bool FindPtpInterface(std::wstring &ptp_instance_id);
  bool DisablePhotoInterface();
  bool EnablePhotoInterface();
  bool ResetUsbDevice();
  bool SetRegistryConfigIndex(const std::wstring &phone_id, uint32_t orig_idx, uint32_t alt_idx);
  bool RemoveRegistryConfigIndex(const std::wstring &phone_id);
  void RemoveAllRegistryConfigIndices(bool full_revert);
  bool RestartDeviceNode(const std::wstring &phone_id);
  bool EnsureLibusbFilterAttached(const std::wstring &phone_id);
  bool DetachAllLibusbFilters();
  bool PerformModeSwitch(int target_mode, const std::wstring &phone_id, std::string &error_msg);
  bool FindNcmAdapter(std::wstring &adapter_name, std::wstring &adapter_desc, uint32_t &if_index, bool &is_up);
  bool FindActiveUplink(std::wstring &uplink_name, uint32_t &uplink_if_index);
  bool ConfigureIcs(const std::wstring &uplink_name, const std::wstring &ncm_name, std::string &error_msg);
  bool DisableAllIcs(std::string &error_msg);
  bool QueryLease(uint32_t if_index, std::string &leased_ip);
  void QueryLinkStats(uint32_t if_index, uint64_t &rx_bytes, uint64_t &tx_bytes);

  std::atomic<bool> monitor_running_{false};
  std::atomic<bool> filter_attached_{false};
  std::thread monitor_thread_;
  std::mutex state_mutex_;
  TetherMonitorState monitor_state_ = TetherMonitorState::Idle;

  bool running_ = false;
  bool was_active_ = false;
  std::wstring current_phone_id_;
  std::wstring current_phone_name_;
  std::wstring current_adapter_name_;
  std::wstring current_uplink_name_;
  uint32_t current_adapter_if_index_ = 0;
  std::string current_leased_ip_;
  std::string last_error_;

  uint64_t last_rx_bytes_ = 0;
  uint64_t last_tx_bytes_ = 0;
  uint64_t last_stats_time_ns_ = 0;
  uint32_t cur_rx_speed_kbps_ = 0;
  uint32_t cur_tx_speed_kbps_ = 0;

  void *libusb_module_ = nullptr;
};

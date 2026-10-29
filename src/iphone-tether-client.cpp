#include "iphone-tether-client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <thread>
#include <sstream>

namespace {

const wchar_t *kPipeName = L"\\\\.\\pipe\\obs-airplay-tether";

bool IsElevated()
{
  BOOL isAdmin = FALSE;
  PSID adminGroup = nullptr;
  SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
  if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                               DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                               &adminGroup)) {
    CheckTokenMembership(nullptr, adminGroup, &isAdmin);
    FreeSid(adminGroup);
  }
  return isAdmin == TRUE;
}

std::string ExtractJsonField(const std::string &json, const std::string &key)
{
  std::string pattern = "\"" + key + "\":\"";
  size_t pos = json.find(pattern);
  if (pos != std::string::npos) {
    size_t start = pos + pattern.size();
    size_t end = json.find("\"", start);
    if (end != std::string::npos) {
      return json.substr(start, end - start);
    }
  }
  return "";
}

uint64_t ExtractJsonUint64(const std::string &json, const std::string &key)
{
  std::string pattern = "\"" + key + "\":";
  size_t pos = json.find(pattern);
  if (pos != std::string::npos) {
    size_t start = pos + pattern.size();
    char *endPtr = nullptr;
    return strtoull(json.c_str() + start, &endPtr, 10);
  }
  return 0;
}

bool ExtractJsonBool(const std::string &json, const std::string &key)
{
  std::string pattern = "\"" + key + "\":";
  size_t pos = json.find(pattern);
  if (pos != std::string::npos) {
    size_t start = pos + pattern.size();
    return json.compare(start, 4, "true") == 0;
  }
  return false;
}

IPhoneTetherStatus ParseStatusJson(const std::string &json)
{
  IPhoneTetherStatus st;
  st.active = ExtractJsonBool(json, "active");
  st.connecting = ExtractJsonBool(json, "connecting");
  st.state_text = ExtractJsonField(json, "state");
  st.phone_name = ExtractJsonField(json, "phone");
  st.adapter_name = ExtractJsonField(json, "adapter");
  st.uplink_name = ExtractJsonField(json, "uplink");
  st.leased_ip = ExtractJsonField(json, "ip");
  st.error_message = ExtractJsonField(json, "error");
  st.rx_bytes = ExtractJsonUint64(json, "rx_mb") * 1024 * 1024;
  st.tx_bytes = ExtractJsonUint64(json, "tx_mb") * 1024 * 1024;
  st.rx_speed_kbps = (uint32_t)ExtractJsonUint64(json, "rx_kbps");
  st.tx_speed_kbps = (uint32_t)ExtractJsonUint64(json, "tx_kbps");
  return st;
}

} // namespace

IPhoneTetherClient &IPhoneTetherClient::Instance()
{
  static IPhoneTetherClient instance;
  return instance;
}

IPhoneTetherClient::IPhoneTetherClient()
{
  // If running in an elevated process, we can execute directly
  if (IsElevated()) {
    use_in_process_ = true;
    in_process_manager_ = std::make_unique<IPhoneTetherManager>();
  }
}

IPhoneTetherClient::~IPhoneTetherClient()
{
  OnObsShutdown();
}

void IPhoneTetherClient::OnObsShutdown()
{
  std::lock_guard<std::mutex> lock(mutex_);

  if (use_in_process_ && in_process_manager_) {
    if (in_process_manager_->NeedsCleanup()) {
      std::string err;
      in_process_manager_->StopShare(true, err);
    }
    is_active_ = false;
    return;
  }

  // If helper pipe exists (even if tethering was stopped earlier in this OBS session),
  // send EXIT without launching a new helper so stock Apple drivers are restored and no process lingers.
  if ((pipe_handle_ && pipe_handle_ != INVALID_HANDLE_VALUE) || WaitNamedPipeW(kPipeName, 50)) {
    std::string resp, err;
    SendPipeCommand("EXIT", false, resp, err);
  }

  if (pipe_handle_ && pipe_handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle((HANDLE)pipe_handle_);
    pipe_handle_ = nullptr;
  }
  is_active_ = false;
}

bool IPhoneTetherClient::LaunchHelperElevated()
{
  // First attempt: trigger scheduled task with highest privilege (no UAC prompt!)
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  wchar_t cmdSchtasks[] = L"schtasks.exe /run /tn \"OBS-AirPlay-Tether-Helper\"";
  if (CreateProcessW(nullptr, cmdSchtasks, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    // Wait for helper to start and open pipe
    for (int i = 0; i < 15; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (WaitNamedPipeW(kPipeName, 200)) {
        return true;
      }
    }
  }

  // Second attempt: Find helper executable and launch via ShellExecute with runas
  std::vector<std::wstring> candidates;

  wchar_t modPath[MAX_PATH] = {0};
  HMODULE hMod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)&IsElevated, &hMod);
  if (hMod && GetModuleFileNameW(hMod, modPath, MAX_PATH)) {
    wchar_t *lastSlash = wcsrchr(modPath, L'\\');
    if (lastSlash) {
      *(lastSlash + 1) = L'\0';
      candidates.push_back(std::wstring(modPath) + L"obs-airplay-tether-helper.exe");
    }
  }

  candidates.push_back(L"C:\\Program Files\\obs-studio\\obs-plugins\\64bit\\obs-airplay-tether-helper.exe");

  wchar_t appdata[MAX_PATH] = {0};
  if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
    candidates.push_back(std::wstring(appdata) + L"\\obs-studio\\plugins\\obs-scrcpy-source\\bin\\64bit\\obs-airplay-tether-helper.exe");
  }

  for (const auto &helperPath : candidates) {
    if (GetFileAttributesW(helperPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
      SHELLEXECUTEINFOW sei = {sizeof(sei)};
      sei.lpVerb = L"runas";
      sei.lpFile = helperPath.c_str();
      sei.lpParameters = L"--run";
      sei.nShow = SW_HIDE;
      sei.fMask = SEE_MASK_NOASYNC;
      if (ShellExecuteExW(&sei)) {
        for (int i = 0; i < 50; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          if (WaitNamedPipeW(kPipeName, 200)) {
            return true;
          }
        }
      }
      break;
    }
  }

  return false;
}

bool IPhoneTetherClient::EnsureConnected(bool allow_launch, std::string &error_msg)
{
  if (pipe_handle_ && pipe_handle_ != INVALID_HANDLE_VALUE) {
    return true;
  }

  // Try to connect to existing pipe
  if (!WaitNamedPipeW(kPipeName, 50)) {
    if (!allow_launch) {
      error_msg = "Tethering helper is idle";
      return false;
    }
    // Only launch elevated helper when user explicitly enables USB Reverse Tethering
    if (!LaunchHelperElevated()) {
      error_msg = "Could not launch elevated reverse tethering helper service";
      return false;
    }
  }

  HANDLE h = CreateFileW(
      kPipeName,
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      0,
      nullptr);

  if (h == INVALID_HANDLE_VALUE) {
    error_msg = "Failed to connect to tethering helper pipe";
    return false;
  }

  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
  pipe_handle_ = (void *)h;
  return true;
}

bool IPhoneTetherClient::SendPipeCommand(const std::string &cmd, bool allow_launch, std::string &response, std::string &error_msg)
{
  if (!EnsureConnected(allow_launch, error_msg)) {
    return false;
  }

  HANDLE h = (HANDLE)pipe_handle_;
  std::string req = cmd + "\n";
  DWORD written = 0;
  if (!WriteFile(h, req.c_str(), (DWORD)req.size(), &written, nullptr)) {
    CloseHandle(h);
    pipe_handle_ = nullptr;
    error_msg = "Pipe write failed";
    return false;
  }

  char buf[2048] = {0};
  DWORD readBytes = 0;
  if (!ReadFile(h, buf, sizeof(buf) - 1, &readBytes, nullptr)) {
    CloseHandle(h);
    pipe_handle_ = nullptr;
    error_msg = "Pipe read failed";
    return false;
  }

  buf[readBytes] = '\0';
  response = buf;
  return true;
}

bool IPhoneTetherClient::StartTethering(std::string &error_msg)
{
  std::lock_guard<std::mutex> lock(mutex_);

  if (use_in_process_ && in_process_manager_) {
    bool ok = in_process_manager_->StartShare(error_msg);
    is_active_ = ok;
    last_status_ = in_process_manager_->GetStatus();
    return ok;
  }

  std::string resp;
  if (!SendPipeCommand("START", true, resp, error_msg)) {
    is_active_ = false;
    last_status_.state_text = "Error: " + error_msg;
    return false;
  }

  last_status_ = ParseStatusJson(resp);
  // Mark session active whenever START succeeded (including while switching mode / awaiting trust)
  // so StopTethering / OBS exit always cleans up back to stock Apple drivers.
  is_active_ = true;
  if (!last_status_.error_message.empty()) {
    error_msg = last_status_.error_message;
  }
  return true;
}

bool IPhoneTetherClient::StopTethering(bool full_revert, std::string &error_msg)
{
  std::lock_guard<std::mutex> lock(mutex_);

  if (use_in_process_ && in_process_manager_) {
    bool ok = in_process_manager_->StopShare(full_revert, error_msg);
    is_active_ = false;
    last_status_ = in_process_manager_->GetStatus();
    return ok;
  }

  if ((pipe_handle_ && pipe_handle_ != INVALID_HANDLE_VALUE) || WaitNamedPipeW(kPipeName, 50)) {
    std::string resp;
    SendPipeCommand(full_revert ? "EXIT" : "STOP", false, resp, error_msg);
    if (pipe_handle_ && pipe_handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle((HANDLE)pipe_handle_);
      pipe_handle_ = nullptr;
    }
  }

  is_active_ = false;
  last_status_ = IPhoneTetherStatus{};
  last_status_.state_text = "Disconnected (Stock Apple USB drivers active)";
  return true;
}

IPhoneTetherStatus IPhoneTetherClient::GetStatus()
{
  std::lock_guard<std::mutex> lock(mutex_);

  if (use_in_process_ && in_process_manager_) {
    last_status_ = in_process_manager_->GetStatus();
    return last_status_;
  }

  std::string resp, err;
  // Never spawn the elevated helper just to read status when tethering is idle
  if (SendPipeCommand("STATUS", false, resp, err)) {
    last_status_ = ParseStatusJson(resp);
    is_active_ = last_status_.active || last_status_.connecting;
  } else if (!is_active_) {
    last_status_ = IPhoneTetherStatus{};
    last_status_.state_text = "Disconnected (Cable idle)";
  }

  return last_status_;
}

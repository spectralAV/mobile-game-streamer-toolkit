#include "iphone-tether.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <netcon.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include "lusb0_usb.h"

// Define function pointers for libusb0.dll
typedef void (*usb_init_t)(void);
typedef int (*usb_find_busses_t)(void);
typedef int (*usb_find_devices_t)(void);
typedef struct usb_bus *(*usb_get_busses_t)(void);
typedef usb_dev_handle *(*usb_open_t)(struct usb_device *dev);
typedef int (*usb_close_t)(usb_dev_handle *dev);
typedef int (*usb_control_msg_t)(usb_dev_handle *dev, int requesttype, int request,
                                 int value, int index, char *bytes, int size, int timeout);
typedef int (*usb_reset_t)(usb_dev_handle *dev);

static usb_init_t fn_usb_init = nullptr;
static usb_find_busses_t fn_usb_find_busses = nullptr;
static usb_find_devices_t fn_usb_find_devices = nullptr;
static usb_get_busses_t fn_usb_get_busses = nullptr;
static usb_open_t fn_usb_open = nullptr;
static usb_close_t fn_usb_close = nullptr;
static usb_control_msg_t fn_usb_control_msg = nullptr;
static usb_reset_t fn_usb_reset = nullptr;

namespace {

const uint32_t kSafeIndex = 2; // config 3 in initial mode: PTP + usbmux
const uint32_t kNcmIndex = 4;  // config 5 after switch: PTP + usbmux + CDC-NCM

std::string WideToUtf8(const std::wstring &wstr)
{
  if (wstr.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
  std::string result(size, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &result[0], size, nullptr, nullptr);
  return result;
}

uint64_t GetCurrentTimeNs()
{
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

bool CaseInsensitiveContains(const std::wstring &str, const std::wstring &sub)
{
  if (sub.empty()) return true;
  if (str.size() < sub.size()) return false;
  auto it = std::search(
      str.begin(), str.end(),
      sub.begin(), sub.end(),
      [](wchar_t ch1, wchar_t ch2) { return towlower(ch1) == towlower(ch2); });
  return it != str.end();
}

bool RunHiddenCommand(const std::wstring &cmd)
{
  STARTUPINFOW si = {sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};

  std::wstring fullCmd = cmd;
  if (fullCmd.rfind(L"pnputil", 0) == 0) {
    wchar_t sysDir[MAX_PATH] = {0};
    if (GetSystemDirectoryW(sysDir, MAX_PATH)) {
      fullCmd = std::wstring(sysDir) + L"\\" + cmd;
    }
  }

  std::vector<wchar_t> cmdLine(fullCmd.begin(), fullCmd.end());
  cmdLine.push_back(L'\0');

  if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
  }
  return false;
}

void LogTether(const std::string &msg)
{
  std::string line = "[TetherManager] " + msg + "\n";
  OutputDebugStringA(line.c_str());
  wchar_t appdata[MAX_PATH] = {0};
  if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
    std::wstring dir = std::wstring(appdata) + L"\\obs-studio\\plugin_config\\obs-scrcpy-source";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring logPath = dir + L"\\tether_helper.log";
    FILE *f = _wfopen(logPath.c_str(), L"a");
    if (f) {
      fputs(line.c_str(), f);
      fclose(f);
    }
  }
}

} // namespace

IPhoneTetherManager::IPhoneTetherManager()
{
  EnsureLibusbLoaded();
  DisablePhotoInterface();
}

IPhoneTetherManager::~IPhoneTetherManager()
{
  monitor_running_ = false;
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
  if (was_active_) {
    std::string err;
    StopShare(true, err);
  }
  if (libusb_module_) {
    FreeLibrary((HMODULE)libusb_module_);
    libusb_module_ = nullptr;
  }
}

bool IPhoneTetherManager::EnsureLibusbLoaded()
{
  if (libusb_module_ && fn_usb_control_msg) {
    return true;
  }

  // Check next to executable/dll first
  wchar_t modPath[MAX_PATH] = {0};
  HMODULE hCurrent = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)&fn_usb_init, &hCurrent);
  if (hCurrent) {
    GetModuleFileNameW(hCurrent, modPath, MAX_PATH);
    wchar_t *lastSlash = wcsrchr(modPath, L'\\');
    if (lastSlash) {
      *(lastSlash + 1) = L'\0';
      std::wstring candidate = std::wstring(modPath) + L"libusb0.dll";
      libusb_module_ = LoadLibraryW(candidate.c_str());
    }
  }

  if (!libusb_module_) {
    libusb_module_ = LoadLibraryW(L"libusb0.dll");
  }

  if (!libusb_module_) {
    return false;
  }

  fn_usb_init = (usb_init_t)GetProcAddress((HMODULE)libusb_module_, "usb_init");
  fn_usb_find_busses = (usb_find_busses_t)GetProcAddress((HMODULE)libusb_module_, "usb_find_busses");
  fn_usb_find_devices = (usb_find_devices_t)GetProcAddress((HMODULE)libusb_module_, "usb_find_devices");
  fn_usb_get_busses = (usb_get_busses_t)GetProcAddress((HMODULE)libusb_module_, "usb_get_busses");
  fn_usb_open = (usb_open_t)GetProcAddress((HMODULE)libusb_module_, "usb_open");
  fn_usb_close = (usb_close_t)GetProcAddress((HMODULE)libusb_module_, "usb_close");
  fn_usb_control_msg = (usb_control_msg_t)GetProcAddress((HMODULE)libusb_module_, "usb_control_msg");
  fn_usb_reset = (usb_reset_t)GetProcAddress((HMODULE)libusb_module_, "usb_reset");

  if (!fn_usb_init || !fn_usb_control_msg || !fn_usb_open || !fn_usb_close) {
    FreeLibrary((HMODULE)libusb_module_);
    libusb_module_ = nullptr;
    return false;
  }

  fn_usb_init();
  return true;
}

bool IPhoneTetherManager::FindIPhoneDevice(std::wstring &instance_id, std::wstring &friendly_name)
{
  ULONG len = 0;
  if (CM_Get_Device_ID_List_SizeW(&len, L"USB", CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS || len <= 1) {
    return false;
  }

  std::vector<wchar_t> buffer(len);
  if (CM_Get_Device_ID_ListW(L"USB", buffer.data(), len, CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) {
    return false;
  }

  for (const wchar_t *p = buffer.data(); *p; p += wcslen(p) + 1) {
    std::wstring s = p;
    // Matches Apple Mobile Device composite ID: USB\VID_05AC&PID_12A* (case-insensitive)
    if (CaseInsensitiveContains(s, L"VID_05AC") &&
        CaseInsensitiveContains(s, L"PID_12A") &&
        !CaseInsensitiveContains(s, L"MI_")) {
      instance_id = p;
      DEVNODE dn;
      if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)p, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
        wchar_t nameBuf[256] = {0};
        ULONG nameLen = sizeof(nameBuf);
        if (CM_Get_DevNode_Registry_PropertyW(dn, CM_DRP_FRIENDLYNAME, nullptr, nameBuf, &nameLen, 0) == CR_SUCCESS && nameBuf[0]) {
          friendly_name = nameBuf;
        } else if (CM_Get_DevNode_Registry_PropertyW(dn, CM_DRP_DEVICEDESC, nullptr, nameBuf, &nameLen, 0) == CR_SUCCESS && nameBuf[0]) {
          friendly_name = nameBuf;
        } else {
          friendly_name = L"Apple iPhone";
        }
      } else {
        friendly_name = L"Apple iPhone";
      }
      return true;
    }
  }

  return false;
}

bool IPhoneTetherManager::EnsureDeviceHealthy(const std::wstring &phone_id)
{
  if (phone_id.empty()) {
    return false;
  }

  DEVNODE dn;
  if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)phone_id.c_str(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, dn, 0) == CR_SUCCESS) {
      if ((status & DN_HAS_PROBLEM) || problem != 0) {
        LogTether("Composite devnode " + WideToUtf8(phone_id) + " has problem (code " +
                  std::to_string(problem) + "). Healing device to safe config 2...");
        SetRegistryConfigIndex(phone_id, kSafeIndex, 0);
        std::wstring disCmd = L"pnputil.exe /disable-device \"" + phone_id + L"\"";
        RunHiddenCommand(disCmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        std::wstring enCmd = L"pnputil.exe /enable-device \"" + phone_id + L"\"";
        RunHiddenCommand(enCmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        return false;
      }
    }
  }
  return true;
}

bool IPhoneTetherManager::FindAllPtpInterfaces(std::vector<std::wstring> &ptp_ids)
{
  ULONG len = 0;
  if (CM_Get_Device_ID_List_SizeW(&len, L"USB", CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS || len <= 1) {
    return false;
  }

  std::vector<wchar_t> buffer(len);
  if (CM_Get_Device_ID_ListW(L"USB", buffer.data(), len, CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) {
    return false;
  }

  for (const wchar_t *p = buffer.data(); *p; p += wcslen(p) + 1) {
    std::wstring s = p;
    if (CaseInsensitiveContains(s, L"05ac") &&
        CaseInsensitiveContains(s, L"12a") &&
        CaseInsensitiveContains(s, L"mi_00")) {
      ptp_ids.push_back(s);
    }
  }

  return !ptp_ids.empty();
}

bool IPhoneTetherManager::FindPtpInterface(std::wstring &ptp_instance_id)
{
  std::vector<std::wstring> ptp_ids;
  if (FindAllPtpInterfaces(ptp_ids) && !ptp_ids.empty()) {
    ptp_instance_id = ptp_ids.front();
    return true;
  }
  return false;
}

bool IPhoneTetherManager::DisablePhotoInterface()
{
  std::vector<std::wstring> ptp_ids;
  FindAllPtpInterfaces(ptp_ids);
  for (const auto &id : ptp_ids) {
    DEVNODE dn;
    if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)id.c_str(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
      CM_Disable_DevNode(dn, CM_DISABLE_UI_NOT_OK);
    }
    std::wstring cmd = L"pnputil.exe /disable-device \"" + id + L"\"";
    RunHiddenCommand(cmd);
  }
  LogTether("DisablePhotoInterface: disabled " + std::to_string(ptp_ids.size()) + " PTP devnodes.");
  return !ptp_ids.empty();
}

void IPhoneTetherManager::RemoveAllRegistryConfigIndices(bool /*full_revert*/)
{
  HKEY hUsb = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ, &hUsb) != ERROR_SUCCESS) {
    return;
  }

  wchar_t subName[256] = {0};
  DWORD index = 0;
  while (RegEnumKeyW(hUsb, index++, subName, 256) == ERROR_SUCCESS) {
    std::wstring s = subName;
    if (CaseInsensitiveContains(s, L"VID_05AC") && CaseInsensitiveContains(s, L"PID_12A") && !CaseInsensitiveContains(s, L"MI_")) {
      std::wstring devKeyPath = L"SYSTEM\\CurrentControlSet\\Enum\\USB\\" + s;
      HKEY hDev = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, devKeyPath.c_str(), 0, KEY_READ, &hDev) == ERROR_SUCCESS) {
        wchar_t instName[256] = {0};
        DWORD instIdx = 0;
        while (RegEnumKeyW(hDev, instIdx++, instName, 256) == ERROR_SUCCESS) {
          std::wstring instKeyPath = devKeyPath + L"\\" + instName;

          // Device Parameters: always restore safe index 2 (PTP + usbmux), NEVER delete down to 1 (camera only)
          std::wstring paramsPath = instKeyPath + L"\\Device Parameters";
          HKEY hParams = nullptr;
          if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, paramsPath.c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hParams) == ERROR_SUCCESS) {
            DWORD valOrig = kSafeIndex;
            DWORD valAlt = 0;
            RegSetValueExW(hParams, L"OriginalConfigurationValue", 0, REG_DWORD, (const BYTE *)&valOrig, sizeof(DWORD));
            RegSetValueExW(hParams, L"AltConfigurationValue", 0, REG_DWORD, (const BYTE *)&valAlt, sizeof(DWORD));
            RegCloseKey(hParams);
          }
        }
        RegCloseKey(hDev);
      }
    }
  }
  RegCloseKey(hUsb);
}

bool IPhoneTetherManager::EnablePhotoInterface()
{
  std::vector<std::wstring> ptp_ids;
  FindAllPtpInterfaces(ptp_ids);
  for (const auto &id : ptp_ids) {
    DEVNODE dn;
    if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)id.c_str(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS ||
        CM_Locate_DevNodeW(&dn, (DEVINSTID_W)id.c_str(), CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS) {
      CM_Enable_DevNode(dn, 0);
    }
    std::wstring cmd = L"pnputil.exe /enable-device \"" + id + L"\"";
    RunHiddenCommand(cmd);
  }
  LogTether("EnablePhotoInterface: re-enabled " + std::to_string(ptp_ids.size()) + " PTP devnodes.");
  return true;
}

bool IPhoneTetherManager::ResetUsbDevice()
{
  if (!EnsureLibusbLoaded() || !fn_usb_reset) {
    return false;
  }

  fn_usb_find_busses();
  fn_usb_find_devices();
  struct usb_bus *busses = fn_usb_get_busses();

  struct usb_device *apple_dev = nullptr;
  for (struct usb_bus *bus = busses; bus && !apple_dev; bus = bus->next) {
    for (struct usb_device *dev = bus->devices; dev; dev = dev->next) {
      if (dev->descriptor.idVendor == 0x05AC) {
        apple_dev = dev;
        break;
      }
    }
  }

  if (!apple_dev) {
    return false;
  }

  usb_dev_handle *handle = fn_usb_open(apple_dev);
  if (!handle) {
    return false;
  }

  fn_usb_reset(handle);
  fn_usb_close(handle);
  return true;
}

bool IPhoneTetherManager::SetRegistryConfigIndex(const std::wstring &phone_id, uint32_t orig_idx, uint32_t alt_idx)
{
  std::wstring subkey = L"SYSTEM\\CurrentControlSet\\Enum\\" + phone_id + L"\\Device Parameters";
  HKEY hKey = nullptr;
  LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, nullptr, 0,
                                   KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &hKey, nullptr);
  if (status != ERROR_SUCCESS) {
    return false;
  }

  DWORD valOrig = orig_idx;
  DWORD valAlt = alt_idx;
  RegSetValueExW(hKey, L"OriginalConfigurationValue", 0, REG_DWORD, (const BYTE *)&valOrig, sizeof(DWORD));
  RegSetValueExW(hKey, L"AltConfigurationValue", 0, REG_DWORD, (const BYTE *)&valAlt, sizeof(DWORD));
  RegCloseKey(hKey);
  return true;
}

bool IPhoneTetherManager::RemoveRegistryConfigIndex(const std::wstring &phone_id)
{
  return SetRegistryConfigIndex(phone_id, kSafeIndex, 0);
}

bool IPhoneTetherManager::RestartDeviceNode(const std::wstring &phone_id)
{
  if (phone_id.empty()) {
    return false;
  }

  LogTether("RestartDeviceNode: restarting " + WideToUtf8(phone_id));
  std::wstring cmd = L"pnputil.exe /restart-device \"" + phone_id + L"\"";
  RunHiddenCommand(cmd);

  DEVNODE dn;
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)phone_id.c_str(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
      ULONG status = 0, problem = 0;
      if (CM_Get_DevNode_Status(&status, &problem, dn, 0) == CR_SUCCESS && !(status & DN_HAS_PROBLEM)) {
        LogTether("RestartDeviceNode: device node settled OK.");
        return true;
      }
    }
  }

  return true;
}

bool IPhoneTetherManager::PerformModeSwitch(int target_mode, std::string &error_msg)
{
  if (!EnsureLibusbLoaded()) {
    error_msg = "libusb0.dll or filter driver is not available";
    return false;
  }

  fn_usb_find_busses();
  fn_usb_find_devices();
  struct usb_bus *busses = fn_usb_get_busses();

  struct usb_device *apple_dev = nullptr;
  for (struct usb_bus *bus = busses; bus && !apple_dev; bus = bus->next) {
    for (struct usb_device *dev = bus->devices; dev; dev = dev->next) {
      if (dev->descriptor.idVendor == 0x05AC) {
        apple_dev = dev;
        break;
      }
    }
  }

  if (!apple_dev) {
    error_msg = "iPhone not found through libusb filter";
    return false;
  }

  usb_dev_handle *handle = fn_usb_open(apple_dev);
  if (!handle) {
    error_msg = "Failed to open iPhone USB handle";
    return false;
  }

  // GET_MODE: 0xC0, 0x45, 0, 0, 4 bytes
  char mode_buf[8] = {0};
  int ret = fn_usb_control_msg(handle, 0xC0, 0x45, 0, 0, mode_buf, 4, 1000);
  if (ret < 4) {
    fn_usb_close(handle);
    error_msg = "GET_MODE control transfer failed (device may be locked with passcode)";
    return false;
  }
  LogTether("GET_MODE returned ret=" + std::to_string(ret) + " mode=" + std::to_string((int)mode_buf[0]));

  // If already in target mode, no need to SET_MODE again
  if (mode_buf[0] == 5 && target_mode == 3) {
    fn_usb_close(handle);
    return true;
  }

  // SET_MODE: 0xC0, 0x52, 0, mode, 1 byte
  char set_buf[4] = {0};
  ret = fn_usb_control_msg(handle, 0xC0, 0x52, 0, target_mode, set_buf, 1, 2000);
  fn_usb_close(handle);

  LogTether("SET_MODE " + std::to_string(target_mode) + " returned ret=" + std::to_string(ret) + " byte=" + std::to_string((int)set_buf[0]));

  if (target_mode == 3 && (ret != 1 || set_buf[0] != 0)) {
    error_msg = "SET_MODE request rejected by iPhone (passcode lock or trust required)";
    return false;
  }

  return true;
}

static bool QueryAdaptersAddressesSafe(std::vector<BYTE> &buffer)
{
  ULONG outLen = 0;
  GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_ALL_INTERFACES, nullptr, nullptr, &outLen);
  if (outLen == 0) outLen = 65536;
  buffer.resize(outLen);
  DWORD ret = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3; ++attempt) {
    ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_ALL_INTERFACES, nullptr,
                               (PIP_ADAPTER_ADDRESSES)buffer.data(), &outLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
      buffer.resize(outLen);
      continue;
    }
    break;
  }
  return ret == NO_ERROR;
}

static bool IsAppleAdapter(const char *adapterGuidName, const std::wstring &desc)
{
  std::wstring descLower = desc;
  for (auto &c : descLower) c = towlower(c);
  if (descLower.find(L"apple") != std::wstring::npos) {
    return true;
  }

  if (!adapterGuidName || !*adapterGuidName) {
    return false;
  }

  std::wstring guidStr;
  const char *p = adapterGuidName;
  while (*p) {
    guidStr.push_back((wchar_t)*p++);
  }

  std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\" +
                         guidStr + L"\\Connection";
  HKEY hKey = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
    wchar_t pnpId[512] = {0};
    DWORD cbData = sizeof(pnpId);
    DWORD dwType = 0;
    if (RegQueryValueExW(hKey, L"PnPInstanceId", nullptr, &dwType, (LPBYTE)pnpId, &cbData) == ERROR_SUCCESS) {
      RegCloseKey(hKey);
      std::wstring pnpLower = pnpId;
      for (auto &c : pnpLower) c = towlower(c);
      if (pnpLower.find(L"vid_05ac") != std::wstring::npos) {
        return true;
      }
    } else {
      RegCloseKey(hKey);
    }
  }

  return false;
}

bool IPhoneTetherManager::FindNcmAdapter(std::wstring &adapter_name, std::wstring &adapter_desc, uint32_t &if_index, bool &is_up)
{
  is_up = false;
  std::vector<BYTE> buffer;
  if (!QueryAdaptersAddressesSafe(buffer)) {
    return false;
  }

  PIP_ADAPTER_ADDRESSES pAddresses = (PIP_ADAPTER_ADDRESSES)buffer.data();

  uint32_t best_if_index = 0;
  std::wstring best_name;
  std::wstring best_desc;
  bool best_is_up = false;
  int best_score = -1;

  for (PIP_ADAPTER_ADDRESSES cur = pAddresses; cur; cur = cur->Next) {
    if (cur->IfType != IF_TYPE_ETHERNET_CSMACD) {
      continue;
    }
    // Skip adapters that are completely absent from PnP
    if (cur->OperStatus == IfOperStatusNotPresent) {
      continue;
    }

    std::wstring desc = cur->Description ? cur->Description : L"";
    std::wstring name = cur->FriendlyName ? cur->FriendlyName : L"";

    // MUST belong to Apple (VID_05AC or Apple in description)
    if (!IsAppleAdapter(cur->AdapterName, desc)) {
      continue;
    }

    const bool up = (cur->OperStatus == IfOperStatusUp);
    int candidate_score = (up ? 10000 : 0);
    int num = 1;
    size_t last_delim = name.find_last_of(L" #");
    if (last_delim != std::wstring::npos && last_delim + 1 < name.size()) {
      try {
        num = std::stoi(name.substr(last_delim + 1));
      } catch (...) {}
    }
    candidate_score += num;

    if (best_score == -1 || candidate_score > best_score) {
      best_score = candidate_score;
      best_if_index = cur->IfIndex;
      best_name = name;
      best_desc = desc;
      best_is_up = up;
    }
  }

  if (best_if_index > 0) {
    adapter_name = best_name;
    adapter_desc = best_desc;
    if_index = best_if_index;
    is_up = best_is_up;
    LogTether("FindNcmAdapter found Apple adapter: [" + WideToUtf8(adapter_name) +
              "] (" + WideToUtf8(adapter_desc) + ") IfIndex=" + std::to_string(if_index) +
              " is_up=" + (best_is_up ? "true" : "false"));
    return true;
  }

  return false;
}

static bool IsValidPhysicalUplink(const std::wstring &friendly_name, const std::wstring &desc, DWORD if_type)
{
  if (if_type != IF_TYPE_ETHERNET_CSMACD && if_type != IF_TYPE_IEEE80211) {
    return false;
  }

  std::wstring fLower = friendly_name;
  for (auto &c : fLower) c = towlower(c);
  std::wstring dLower = desc;
  for (auto &c : dLower) c = towlower(c);

  const std::wstring blacklist[] = {
      L"apple", L"usbncm", L"cdc ncm", L"tailscale", L"zerotier",
      L"wireguard", L"openvpn", L"tap-", L"cisco", L"anyconnect",
      L"nordvpn", L"proton", L"warp", L"virtual", L"hyper-v",
      L"vmware", L"virtualbox", L"bluetooth", L"wi-fi direct",
      L"wan miniport", L"loopback", L"kernel debug", L"npcap",
      L"wsl", L"docker", L"vethernet", L"teredo", L"isatap", L"6to4"
  };

  for (const auto &bad : blacklist) {
    if (fLower.find(bad) != std::wstring::npos || dLower.find(bad) != std::wstring::npos) {
      return false;
    }
  }

  return true;
}

bool IPhoneTetherManager::FindActiveUplink(std::wstring &uplink_name, uint32_t &uplink_if_index)
{
  // 1. Try GetBestRoute to 8.8.8.8 to find current primary uplink
  uint32_t target_if = 0;
  MIB_IPFORWARDROW route = {};
  in_addr destAddr = {};
  inet_pton(AF_INET, "8.8.8.8", &destAddr);
  if (GetBestRoute(destAddr.s_addr, 0, &route) == NO_ERROR) {
    target_if = route.dwForwardIfIndex;
  }

  std::vector<BYTE> buffer;
  if (!QueryAdaptersAddressesSafe(buffer)) {
    return false;
  }

  PIP_ADAPTER_ADDRESSES pAddresses = (PIP_ADAPTER_ADDRESSES)buffer.data();

  // 1. If best route points to a valid physical uplink, use it
  if (target_if > 0) {
    for (PIP_ADAPTER_ADDRESSES cur = pAddresses; cur; cur = cur->Next) {
      if (cur->IfIndex == target_if && cur->OperStatus == IfOperStatusUp) {
        std::wstring name = cur->FriendlyName ? cur->FriendlyName : L"";
        std::wstring desc = cur->Description ? cur->Description : L"";
        if (IsValidPhysicalUplink(name, desc, cur->IfType)) {
          uplink_name = name;
          uplink_if_index = target_if;
          LogTether("FindActiveUplink matched default route uplink: [" + WideToUtf8(uplink_name) + "] (" + WideToUtf8(desc) + ")");
          return true;
        }
      }
    }
  }

  // 2. Fallback: Search all UP physical adapters, prioritizing one with a gateway
  uint32_t best_if = 0;
  std::wstring best_name, best_desc;
  int best_score = -1;

  for (PIP_ADAPTER_ADDRESSES cur = pAddresses; cur; cur = cur->Next) {
    if (cur->OperStatus == IfOperStatusUp) {
      std::wstring name = cur->FriendlyName ? cur->FriendlyName : L"";
      std::wstring desc = cur->Description ? cur->Description : L"";
      if (IsValidPhysicalUplink(name, desc, cur->IfType)) {
        int score = 0;
        if (cur->FirstGatewayAddress != nullptr) {
          score += 100;
        }
        if (cur->IfType == IF_TYPE_ETHERNET_CSMACD) {
          score += 10;
        } else if (cur->IfType == IF_TYPE_IEEE80211) {
          score += 5;
        }
        if (score > best_score) {
          best_score = score;
          best_if = cur->IfIndex;
          best_name = name;
          best_desc = desc;
        }
      }
    }
  }

  if (best_if > 0) {
    uplink_name = best_name;
    uplink_if_index = best_if;
    LogTether("FindActiveUplink selected best physical uplink: [" + WideToUtf8(uplink_name) + "] (" + WideToUtf8(best_desc) + ") score=" + std::to_string(best_score));
    return true;
  }

  return false;
}

static CLSID GetHNetShareClsid()
{
  CLSID clsid = {};
  if (FAILED(CLSIDFromProgID(L"HNetCfg.HNetShare", &clsid))) {
    // Known CLSID {5C63C1AD-3956-4FF8-8486-40034758315B} for hnetcfg.dll
    clsid = {0x5C63C1AD, 0x3956, 0x4FF8, {0x84, 0x86, 0x40, 0x03, 0x47, 0x58, 0x31, 0x5B}};
  }
  return clsid;
}

bool IPhoneTetherManager::ConfigureIcs(const std::wstring &uplink_name, const std::wstring &ncm_name, std::string &error_msg)
{
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool co_inited = SUCCEEDED(hr);

  CoInitializeSecurity(
      nullptr,
      -1,
      nullptr,
      nullptr,
      RPC_C_AUTHN_LEVEL_DEFAULT,
      RPC_C_IMP_LEVEL_IMPERSONATE,
      nullptr,
      EOAC_NONE,
      nullptr);

  INetSharingManager *pNetShareMgr = nullptr;
  hr = CoCreateInstance(GetHNetShareClsid(), nullptr, CLSCTX_ALL, IID_INetSharingManager, (void **)&pNetShareMgr);
  if (FAILED(hr) || !pNetShareMgr) {
    error_msg = "CoCreateInstance(HNetCfg.HNetShare) failed (requires elevation)";
    if (co_inited) CoUninitialize();
    return false;
  }

  INetConnection *uplinkConn = nullptr;
  INetConnection *ncmConn = nullptr;
  std::wstring target_ncm_name = ncm_name;

  // ICS indexing can take a few seconds after NCM mode switch: retry up to 15 times
  for (int attempt = 0; attempt < 15; ++attempt) {
    if (attempt > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    INetSharingEveryConnectionCollection *pCollection = nullptr;
    hr = pNetShareMgr->get_EnumEveryConnection(&pCollection);
    if (FAILED(hr) || !pCollection) {
      continue;
    }

    IEnumVARIANT *pEnum = nullptr;
    hr = pCollection->get__NewEnum((IUnknown **)&pEnum);
    if (FAILED(hr) || !pEnum) {
      pCollection->Release();
      continue;
    }

    VARIANT var;
    VariantInit(&var);
    std::vector<std::pair<INetConnection *, NETCON_PROPERTIES *>> activeList;

    while (pEnum->Next(1, &var, nullptr) == S_OK) {
      if (var.vt == VT_UNKNOWN || var.vt == VT_DISPATCH) {
        INetConnection *pConn = nullptr;
        if (SUCCEEDED(var.punkVal->QueryInterface(IID_INetConnection, (void **)&pConn)) && pConn) {
          NETCON_PROPERTIES *pProps = nullptr;
          if (SUCCEEDED(pConn->GetProperties(&pProps)) && pProps) {
            activeList.push_back({pConn, pProps});
          } else {
            pConn->Release();
          }
        }
      }
      VariantClear(&var);
    }
    pEnum->Release();
    pCollection->Release();

    LogTether("ConfigureIcs attempt " + std::to_string(attempt + 1) + " scanning " +
              std::to_string(activeList.size()) + " ICS connections. Target uplink=[" +
              WideToUtf8(uplink_name) + "], target ncm=[" + WideToUtf8(ncm_name) + "]");
    for (auto &item : activeList) {
      std::wstring cName = item.second->pszwName ? item.second->pszwName : L"";
      std::wstring dName = item.second->pszwDeviceName ? item.second->pszwDeviceName : L"";
      LogTether("  ICS conn: [" + WideToUtf8(cName) + "] Device: [" + WideToUtf8(dName) + "]");
    }

    // Pass 1: Exact Name Match
    for (auto &item : activeList) {
      std::wstring cName = item.second->pszwName ? item.second->pszwName : L"";
      if (!uplinkConn && _wcsicmp(cName.c_str(), uplink_name.c_str()) == 0) {
        uplinkConn = item.first;
        uplinkConn->AddRef();
        LogTether("  Matched uplinkConn (exact): [" + WideToUtf8(cName) + "]");
      }
      if (!ncmConn && _wcsicmp(cName.c_str(), ncm_name.c_str()) == 0) {
        ncmConn = item.first;
        ncmConn->AddRef();
        LogTether("  Matched ncmConn (exact): [" + WideToUtf8(cName) + "]");
      }
    }

    // Pass 2: Case-insensitive fallback for matching adapters
    if (!uplinkConn || !ncmConn) {
      for (auto &item : activeList) {
        std::wstring cName = item.second->pszwName ? item.second->pszwName : L"";
        std::wstring dName = item.second->pszwDeviceName ? item.second->pszwDeviceName : L"";

        if (!uplinkConn && CaseInsensitiveContains(cName, uplink_name)) {
          uplinkConn = item.first;
          uplinkConn->AddRef();
          LogTether("  Matched uplinkConn (fallback): [" + WideToUtf8(cName) + "]");
        }
        if (!ncmConn && item.first != uplinkConn &&
            (CaseInsensitiveContains(cName, ncm_name) ||
             CaseInsensitiveContains(dName, L"usbncm") ||
             CaseInsensitiveContains(dName, L"apple mobile device ethernet") ||
             (CaseInsensitiveContains(dName, L"apple") && CaseInsensitiveContains(dName, L"ethernet")))) {
          ncmConn = item.first;
          ncmConn->AddRef();
          target_ncm_name = cName;
          LogTether("  Matched ncmConn (fallback): [" + WideToUtf8(cName) + "] (" + WideToUtf8(dName) + ")");
        }
      }
    }

    // Clean up properties and references in activeList
    for (auto &item : activeList) {
      CoTaskMemFree(item.second->pszwName);
      CoTaskMemFree(item.second->pszwDeviceName);
      CoTaskMemFree(item.second);
      item.first->Release();
    }

    if (uplinkConn && ncmConn) {
      break;
    }

    if (uplinkConn) {
      uplinkConn->Release();
      uplinkConn = nullptr;
    }
    if (ncmConn) {
      ncmConn->Release();
      ncmConn = nullptr;
    }
  }

  if (!uplinkConn || !ncmConn) {
    if (uplinkConn) uplinkConn->Release();
    if (ncmConn) ncmConn->Release();
    pNetShareMgr->Release();
    error_msg = "Could not locate matching network adapters in ICS";
    LogTether("ConfigureIcs failed: " + error_msg);
    if (co_inited) CoUninitialize();
    return false;
  }

  // Clear existing sharing on other connections
  INetSharingEveryConnectionCollection *pCheckCol = nullptr;
  if (SUCCEEDED(pNetShareMgr->get_EnumEveryConnection(&pCheckCol)) && pCheckCol) {
    IEnumVARIANT *pCheckEnum = nullptr;
    if (SUCCEEDED(pCheckCol->get__NewEnum((IUnknown **)&pCheckEnum)) && pCheckEnum) {
      VARIANT v;
      VariantInit(&v);
      while (pCheckEnum->Next(1, &v, nullptr) == S_OK) {
        if (v.vt == VT_UNKNOWN || v.vt == VT_DISPATCH) {
          INetConnection *pC = nullptr;
          if (SUCCEEDED(v.punkVal->QueryInterface(IID_INetConnection, (void **)&pC)) && pC) {
            INetSharingConfiguration *pCfg = nullptr;
            if (SUCCEEDED(pNetShareMgr->get_INetSharingConfigurationForINetConnection(pC, &pCfg)) && pCfg) {
              VARIANT_BOOL isShared = VARIANT_FALSE;
              if (SUCCEEDED(pCfg->get_SharingEnabled(&isShared)) && isShared == VARIANT_TRUE) {
                pCfg->DisableSharing();
              }
              pCfg->Release();
            }
            pC->Release();
          }
        }
        VariantClear(&v);
      }
      pCheckEnum->Release();
    }
    pCheckCol->Release();
  }

  // Configure Public sharing on Uplink
  INetSharingConfiguration *pUplinkConfig = nullptr;
  if (SUCCEEDED(pNetShareMgr->get_INetSharingConfigurationForINetConnection(uplinkConn, &pUplinkConfig)) && pUplinkConfig) {
    pUplinkConfig->EnableSharing(tagSHARINGCONNECTIONTYPE::ICSSHARINGTYPE_PUBLIC);
    pUplinkConfig->Release();
  }

  // Configure Private sharing on iPhone NCM adapter
  INetSharingConfiguration *pNcmConfig = nullptr;
  if (SUCCEEDED(pNetShareMgr->get_INetSharingConfigurationForINetConnection(ncmConn, &pNcmConfig)) && pNcmConfig) {
    pNcmConfig->EnableSharing(tagSHARINGCONNECTIONTYPE::ICSSHARINGTYPE_PRIVATE);
    pNcmConfig->Release();
  }

  uplinkConn->Release();
  ncmConn->Release();
  pNetShareMgr->Release();

  // Bounce link so iOS DHCP client re-requests lease immediately
  std::wstring bounceCmd = L"netsh.exe interface set interface name=\"" + target_ncm_name + L"\" admin=disable";
  RunHiddenCommand(bounceCmd);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  bounceCmd = L"netsh.exe interface set interface name=\"" + target_ncm_name + L"\" admin=enable";
  RunHiddenCommand(bounceCmd);

  if (co_inited) CoUninitialize();
  return true;
}

bool IPhoneTetherManager::DisableAllIcs(std::string &error_msg)
{
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool co_inited = SUCCEEDED(hr);

  INetSharingManager *pNetShareMgr = nullptr;
  hr = CoCreateInstance(GetHNetShareClsid(), nullptr, CLSCTX_ALL, IID_INetSharingManager, (void **)&pNetShareMgr);
  if (FAILED(hr) || !pNetShareMgr) {
    error_msg = "CoCreateInstance failed for ICS teardown";
    if (co_inited) CoUninitialize();
    return false;
  }

  INetSharingEveryConnectionCollection *pCollection = nullptr;
  if (SUCCEEDED(pNetShareMgr->get_EnumEveryConnection(&pCollection)) && pCollection) {
    IEnumVARIANT *pEnum = nullptr;
    if (SUCCEEDED(pCollection->get__NewEnum((IUnknown **)&pEnum)) && pEnum) {
      VARIANT var;
      VariantInit(&var);
      while (pEnum->Next(1, &var, nullptr) == S_OK) {
        if (var.vt == VT_UNKNOWN || var.vt == VT_DISPATCH) {
          INetConnection *pConn = nullptr;
          if (SUCCEEDED(var.punkVal->QueryInterface(IID_INetConnection, (void **)&pConn)) && pConn) {
            INetSharingConfiguration *pConfig = nullptr;
            if (SUCCEEDED(pNetShareMgr->get_INetSharingConfigurationForINetConnection(pConn, &pConfig)) && pConfig) {
              VARIANT_BOOL isEnabled = VARIANT_FALSE;
              if (SUCCEEDED(pConfig->get_SharingEnabled(&isEnabled)) && isEnabled == VARIANT_TRUE) {
                pConfig->DisableSharing();
              }
              pConfig->Release();
            }
            pConn->Release();
          }
        }
        VariantClear(&var);
      }
      pEnum->Release();
    }
    pCollection->Release();
  }
  pNetShareMgr->Release();

  if (co_inited) CoUninitialize();
  return true;
}

bool IPhoneTetherManager::QueryLease(uint32_t if_index, std::string &leased_ip)
{
  PMIB_IPNET_TABLE2 pTable = nullptr;
  if (GetIpNetTable2(AF_INET, &pTable) != NO_ERROR || !pTable) {
    return false;
  }

  for (ULONG i = 0; i < pTable->NumEntries; ++i) {
    const MIB_IPNET_ROW2 &row = pTable->Table[i];
    if (row.InterfaceIndex == if_index) {
      char ipStr[64] = {0};
      sockaddr_in *sin = (sockaddr_in *)&row.Address;
      inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));

      // Check if address belongs to 192.168.137.x and is not .1 or .255
      std::string ip = ipStr;
      if (ip.rfind("192.168.137.", 0) == 0 && ip != "192.168.137.1" && ip != "192.168.137.255") {
        if (row.PhysicalAddressLength == 6 &&
            !(row.PhysicalAddress[0] == 0 && row.PhysicalAddress[1] == 0 &&
              row.PhysicalAddress[2] == 0 && row.PhysicalAddress[3] == 0)) {
          leased_ip = ip;
          FreeMibTable(pTable);
          return true;
        }
      }
    }
  }

  FreeMibTable(pTable);
  return false;
}

void IPhoneTetherManager::QueryLinkStats(uint32_t if_index, uint64_t &rx_bytes, uint64_t &tx_bytes)
{
  MIB_IF_ROW2 row = {};
  row.InterfaceIndex = if_index;
  if (GetIfEntry2(&row) == NO_ERROR) {
    rx_bytes = row.InOctets;
    tx_bytes = row.OutOctets;
  }
}

void IPhoneTetherManager::MonitorLoop()
{
  LogTether("Background monitor loop started.");
  int loop_count = 0;

  while (monitor_running_) {
    ++loop_count;

    // 1. Detect if an iPhone is physically connected via USB
    std::wstring phone_id, phone_name;
    if (!FindIPhoneDevice(phone_id, phone_name)) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        monitor_state_ = TetherMonitorState::Disconnected;
        last_error_ = "No iPhone detected on USB. Connect cable & unlock screen.";
        running_ = false;
        current_phone_id_.clear();
        current_phone_name_.clear();
        current_adapter_name_.clear();
        current_adapter_if_index_ = 0;
        current_leased_ip_.clear();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      current_phone_id_ = phone_id;
      current_phone_name_ = phone_name;
    }

    // 1b. Ensure composite device is healthy (clear any Code 10 / CM_PROB_FAILED_START)
    if (!EnsureDeviceHealthy(phone_id)) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        monitor_state_ = TetherMonitorState::SwitchingMode;
        last_error_ = "Recovering iPhone USB device state...";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }

    // 2. Check if Apple NCM adapter is present AND active
    std::wstring ncm_name, ncm_desc;
    uint32_t ncm_if_index = 0;
    bool is_ncm_up = false;
    bool has_ncm = FindNcmAdapter(ncm_name, ncm_desc, ncm_if_index, is_ncm_up);

    if (!has_ncm || !is_ncm_up) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        monitor_state_ = TetherMonitorState::SwitchingMode;
        last_error_.clear();
      }

      LogTether("Apple NCM adapter not active (has_ncm=" + std::to_string(has_ncm) +
                ", is_up=" + std::to_string(is_ncm_up) + "). Performing mode switch sequence...");

      SetRegistryConfigIndex(phone_id, kNcmIndex, kSafeIndex);

      std::string mode_err;
      if (!PerformModeSwitch(3, mode_err)) {
        LogTether("PerformModeSwitch(3) failed: " + mode_err);
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          if (mode_err.find("passcode") != std::string::npos || mode_err.find("rejected") != std::string::npos) {
            monitor_state_ = TetherMonitorState::WaitingTrust;
            last_error_ = "Action Required: Please unlock iPhone & tap 'Trust This Computer' on screen";
          } else {
            monitor_state_ = TetherMonitorState::SwitchingMode;
            last_error_ = "USB Mode switch in progress (" + mode_err + ")...";
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        continue;
      }

      // Allow iPhone hardware to re-enumerate on USB and Windows PnP to discover the NCM adapter (up to 10 seconds)
      LogTether("SET_MODE 3 sent. Waiting up to 10s for Apple NCM adapter to enumerate and come Up...");
      for (int i = 0; i < 20 && monitor_running_; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (FindNcmAdapter(ncm_name, ncm_desc, ncm_if_index, is_ncm_up) && is_ncm_up) {
          has_ncm = true;
          break;
        }
      }

      // If NCM adapter still not active, enable any disabled child NCM functions
      if ((!has_ncm || !is_ncm_up) && monitor_running_) {
        LogTether("Apple NCM adapter not yet Up. Enabling any disabled NCM child devnodes...");
        std::wstring enCmd = L"pnputil.exe /enable-device \"USB\\VID_05AC&PID_12A8&MI_02\\*\"";
        RunHiddenCommand(enCmd);
        enCmd = L"pnputil.exe /enable-device \"USB\\VID_05AC&PID_12A8&REV_1404&CDC_0D&MI_02\\*\"";
        RunHiddenCommand(enCmd);
        for (int i = 0; i < 6 && monitor_running_; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          if (FindNcmAdapter(ncm_name, ncm_desc, ncm_if_index, is_ncm_up) && is_ncm_up) {
            has_ncm = true;
            break;
          }
        }
      }

      if (!has_ncm || !is_ncm_up) {
        LogTether("Apple NCM adapter did not come up after 13s. Resetting registry to safe index 2.");
        SetRegistryConfigIndex(phone_id, kSafeIndex, 0);
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          monitor_state_ = TetherMonitorState::Error;
          last_error_ = "USB Ethernet did not respond. Please reconnect USB cable.";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        continue;
      }
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      current_adapter_name_ = ncm_name;
      current_adapter_if_index_ = ncm_if_index;
    }

    // Disable photo interface while tethering is active to suppress photo access popups completely!
    DisablePhotoInterface();

    // Ensure adapter is enabled
    std::wstring enableCmd = L"netsh.exe interface set interface name=\"" + ncm_name + L"\" admin=enable";
    RunHiddenCommand(enableCmd);

    // 4. Find active internet uplink (Wi-Fi / Ethernet)
    std::wstring uplink_name;
    uint32_t uplink_if_index = 0;
    if (!FindActiveUplink(uplink_name, uplink_if_index)) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        monitor_state_ = TetherMonitorState::Error;
        last_error_ = "No active internet connection (Wi-Fi / Ethernet) found on PC to share";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2000));
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      current_uplink_name_ = uplink_name;
    }

    // 5. Configure ICS if not active
    if (!running_) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        monitor_state_ = TetherMonitorState::ConfiguringIcs;
      }

      std::string ics_err;
      if (!ConfigureIcs(uplink_name, ncm_name, ics_err)) {
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          monitor_state_ = TetherMonitorState::Error;
          last_error_ = ics_err;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        running_ = true;
        was_active_ = true;
      }
    }

    // 6. Active state: query DHCP lease and link stats
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (current_leased_ip_.empty()) {
        monitor_state_ = TetherMonitorState::WaitingDhcp;
        QueryLease(ncm_if_index, current_leased_ip_);
      } else {
        monitor_state_ = TetherMonitorState::Active;
        last_error_.clear();
      }
    }

    // Update throughput stats
    uint64_t cur_rx = 0, cur_tx = 0;
    QueryLinkStats(ncm_if_index, cur_rx, cur_tx);
    uint64_t now_ns = GetCurrentTimeNs();
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (last_stats_time_ns_ > 0 && now_ns > last_stats_time_ns_) {
        double dt_sec = (now_ns - last_stats_time_ns_) / 1e9;
        if (dt_sec >= 0.5) {
          if (cur_rx >= last_rx_bytes_) {
            cur_rx_speed_kbps_ = (uint32_t)((cur_rx - last_rx_bytes_) / (dt_sec * 1024.0));
          }
          if (cur_tx >= last_tx_bytes_) {
            cur_tx_speed_kbps_ = (uint32_t)((cur_tx - last_tx_bytes_) / (dt_sec * 1024.0));
          }
          last_rx_bytes_ = cur_rx;
          last_tx_bytes_ = cur_tx;
          last_stats_time_ns_ = now_ns;
        }
      } else {
        last_rx_bytes_ = cur_rx;
        last_tx_bytes_ = cur_tx;
        last_stats_time_ns_ = now_ns;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }

  LogTether("Background monitor loop ended.");
}

bool IPhoneTetherManager::StartShare(std::string &error_msg)
{
  if (monitor_running_) {
    return true;
  }

  monitor_running_ = true;
  monitor_state_ = TetherMonitorState::SwitchingMode;
  last_error_.clear();

  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
  monitor_thread_ = std::thread(&IPhoneTetherManager::MonitorLoop, this);

  // Wait briefly (up to 2 seconds) for initial monitor loop status
  for (int i = 0; i < 20 && monitor_running_; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (monitor_state_ == TetherMonitorState::Active) {
      return true;
    }
    if (monitor_state_ == TetherMonitorState::WaitingTrust || monitor_state_ == TetherMonitorState::Disconnected) {
      break;
    }
  }

  return true;
}

bool IPhoneTetherManager::StopShare(bool full_revert, std::string &error_msg)
{
  LogTether("StopShare called. Disabling ICS and releasing link...");

  monitor_running_ = false;
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }

  // 1. Disable all ICS network sharing
  DisableAllIcs(error_msg);

  // 2. Drop carrier on NCM adapter via SET_MODE 2 vendor request
  std::string mode_err;
  PerformModeSwitch(2, mode_err);

  // 3. Keep Photo (PTP) interface disabled so Windows never prompts for photo access on replug
  DisablePhotoInterface();

  // 4. Restore safe registry configuration override (index 2: PTP + usbmux)
  RemoveAllRegistryConfigIndices(full_revert);

  // 5. Ensure composite device is healthy (clear any Code 10)
  if (!current_phone_id_.empty()) {
    EnsureDeviceHealthy(current_phone_id_);
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    running_ = false;
    was_active_ = false;
    monitor_state_ = TetherMonitorState::Idle;
    current_adapter_name_.clear();
    current_adapter_if_index_ = 0;
    current_leased_ip_.clear();
    last_error_.clear();
  }
  return true;
}

IPhoneTetherStatus IPhoneTetherManager::GetStatus()
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  IPhoneTetherStatus st;
  st.active = running_;
  st.connecting = monitor_running_ && !running_;
  st.phone_name = WideToUtf8(current_phone_name_);
  st.phone_id = WideToUtf8(current_phone_id_);
  st.adapter_name = WideToUtf8(current_adapter_name_);
  st.uplink_name = WideToUtf8(current_uplink_name_);
  st.leased_ip = current_leased_ip_;
  st.error_message = last_error_;
  st.rx_bytes = last_rx_bytes_;
  st.tx_bytes = last_tx_bytes_;
  st.rx_speed_kbps = cur_rx_speed_kbps_;
  st.tx_speed_kbps = cur_tx_speed_kbps_;

  switch (monitor_state_) {
    case TetherMonitorState::Idle:
      st.state_text = "Disconnected (Cable idle)";
      break;
    case TetherMonitorState::Disconnected:
      st.state_text = "Disconnected: Connect iPhone via USB cable";
      break;
    case TetherMonitorState::WaitingTrust:
      st.state_text = "Action Required: Please unlock iPhone & tap 'Trust This Computer' on screen";
      break;
    case TetherMonitorState::SwitchingMode:
      st.state_text = "Switching iPhone to USB Ethernet (CDC-NCM) mode...";
      break;
    case TetherMonitorState::ConfiguringIcs:
      st.state_text = "Configuring Internet Connection Sharing (" + WideToUtf8(current_uplink_name_) + " -> iPhone)...";
      break;
    case TetherMonitorState::WaitingDhcp:
      st.state_text = "Waiting for iPhone IP lease (DHCP)...";
      break;
    case TetherMonitorState::Active: {
      std::ostringstream ss;
      ss << "Active: " << (st.phone_name.empty() ? "iPhone" : st.phone_name)
         << " (" << (st.leased_ip.empty() ? "DHCP Negotiating..." : st.leased_ip)
         << " via " << st.uplink_name << ")";
      if (cur_rx_speed_kbps_ > 0 || cur_tx_speed_kbps_ > 0) {
        if (cur_rx_speed_kbps_ >= 1024) {
          ss << " | " << std::fixed << std::setprecision(1) << (cur_rx_speed_kbps_ / 1024.0) << " MB/s Rx";
        } else {
          ss << " | " << cur_rx_speed_kbps_ << " KB/s Rx";
        }
      }
      st.state_text = ss.str();
      break;
    }
    case TetherMonitorState::Error:
      st.state_text = "Error: " + last_error_;
      break;
  }

  return st;
}

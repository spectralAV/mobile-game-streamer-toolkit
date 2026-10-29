#include "iphone-tether.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <atomic>
#include <thread>

namespace {

const wchar_t *kPipeName = L"\\\\.\\pipe\\obs-airplay-tether";
std::atomic<bool> g_keep_running{true};

void LogMsg(const std::string &msg)
{
  std::string line = "[Helper] " + msg + "\n";
  OutputDebugStringA(line.c_str());
  std::cout << line;

  wchar_t appdata[MAX_PATH] = {0};
  if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
    std::wstring dir = std::wstring(appdata) + L"\\obs-studio\\plugin_config\\obs-scrcpy-source";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring logPath = dir + L"\\tether_helper.log";
    std::ofstream ofs(logPath, std::ios::app);
    if (ofs.is_open()) {
      ofs << line;
    }
  }
}

std::string StatusToJson(const IPhoneTetherStatus &st)
{
  std::ostringstream ss;
  ss << "{"
     << "\"active\":" << (st.active ? "true" : "false") << ","
     << "\"state\":\"" << st.state_text << "\","
     << "\"phone\":\"" << st.phone_name << "\","
     << "\"adapter\":\"" << st.adapter_name << "\","
     << "\"uplink\":\"" << st.uplink_name << "\","
     << "\"ip\":\"" << st.leased_ip << "\","
     << "\"rx_mb\":" << (st.rx_bytes / (1024 * 1024)) << ","
     << "\"tx_mb\":" << (st.tx_bytes / (1024 * 1024)) << ","
     << "\"rx_kbps\":" << st.rx_speed_kbps << ","
     << "\"tx_kbps\":" << st.tx_speed_kbps << ","
     << "\"error\":\"" << st.error_message << "\""
     << "}\n";
  return ss.str();
}

BOOL WINAPI ConsoleHandler(DWORD signal)
{
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    g_keep_running = false;
    return TRUE;
  }
  return FALSE;
}

int RunPipeServer()
{
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
  LogMsg("Starting background named pipe server...");

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = FALSE;
  // D:(A;;GA;;;WD) -> Discretionary ACL: Allow Generic All to World (Everyone) so non-admin OBS can connect
  ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);

  HANDLE hPipe = CreateNamedPipeW(
      kPipeName,
      PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
      PIPE_UNLIMITED_INSTANCES,
      4096,
      4096,
      0,
      &sa);

  if (sa.lpSecurityDescriptor) {
    LocalFree(sa.lpSecurityDescriptor);
  }

  if (hPipe == INVALID_HANDLE_VALUE) {
    LogMsg("Failed to create named pipe: " + std::to_string(GetLastError()));
    return 1;
  }

  LogMsg("Named pipe server ready: \\\\.\\pipe\\obs-airplay-tether");

  IPhoneTetherManager manager;

  while (g_keep_running) {
    BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }

    LogMsg("OBS client connected.");

    char inBuf[1024] = {0};
    DWORD bytesRead = 0;

    while (g_keep_running) {
      BOOL success = ReadFile(hPipe, inBuf, sizeof(inBuf) - 1, &bytesRead, nullptr);
      if (!success || bytesRead == 0) {
        LogMsg("Client disconnected.");
        if (manager.IsRunning() || manager.WasActive()) {
          LogMsg("Reverting reverse tethering to safe defaults...");
          std::string err;
          manager.StopShare(false, err);
        }
        break;
      }

      inBuf[bytesRead] = '\0';
      std::string cmd = inBuf;
      while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n' || cmd.back() == ' ')) {
        cmd.pop_back();
      }

      LogMsg("Received command: " + cmd);

      std::string response;
      if (cmd == "START") {
        std::string err;
        bool ok = manager.StartShare(err);
        LogMsg(std::string("StartShare result: ") + (ok ? "SUCCESS" : ("FAILED: " + err)));
        response = StatusToJson(manager.GetStatus());
      } else if (cmd == "STOP") {
        std::string err;
        manager.StopShare(false, err);
        LogMsg("StopShare executed.");
        response = StatusToJson(manager.GetStatus());
      } else if (cmd == "STATUS") {
        response = StatusToJson(manager.GetStatus());
      } else if (cmd == "EXIT") {
        std::string err;
        if (manager.IsRunning() || manager.WasActive()) {
          manager.StopShare(false, err);
        }
        LogMsg("EXIT received. Exiting helper.");
        response = "{\"status\":\"exiting\"}\n";
        DWORD written = 0;
        WriteFile(hPipe, response.c_str(), (DWORD)response.size(), &written, nullptr);
        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        return 0;
      } else {
        response = "{\"error\":\"unknown command\"}\n";
      }

      DWORD bytesWritten = 0;
      WriteFile(hPipe, response.c_str(), (DWORD)response.size(), &bytesWritten, nullptr);
      FlushFileBuffers(hPipe);
    }

    DisconnectNamedPipe(hPipe);
  }

  CloseHandle(hPipe);
  return 0;
}

} // namespace

int main(int argc, char *argv[])
{
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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

  std::string mode = "--run";
  if (argc > 1) {
    mode = argv[1];
  }

  if (mode != "--run") {
    AttachConsole(ATTACH_PARENT_PROCESS);
  }

  if (mode == "--start") {
    IPhoneTetherManager manager;
    std::string err;
    if (manager.StartShare(err)) {
      std::cout << "SUCCESS: " << manager.GetStatus().state_text << std::endl;
      return 0;
    } else {
      std::cerr << "FAILED: " << err << std::endl;
      return 1;
    }
  } else if (mode == "--stop") {
    IPhoneTetherManager manager;
    std::string err;
    manager.StopShare(true, err);
    std::cout << "STOPPED: Reverted Windows configuration to defaults." << std::endl;
    return 0;
  } else if (mode == "--status") {
    IPhoneTetherManager manager;
    std::cout << StatusToJson(manager.GetStatus());
    return 0;
  }

  return RunPipeServer();
}

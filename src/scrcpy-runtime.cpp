#include "scrcpy-runtime.h"

#include <obs-module.h>
#include <media-io/video-io.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace {

constexpr uintptr_t kInvalidSocket = UINTPTR_MAX;
constexpr int kDeviceNameFieldLength = 64;
constexpr int kPacketHeaderSize = 12;
constexpr uint64_t kPacketFlagSession = UINT64_C(1) << 63;
constexpr uint64_t kPacketFlagConfig = UINT64_C(1) << 62;
constexpr uint64_t kPacketFlagKeyFrame = UINT64_C(1) << 61;
constexpr uint64_t kPacketPtsMask = kPacketFlagKeyFrame - 1;

constexpr uint32_t kCodecH264 = 0x68323634;
constexpr uint32_t kCodecH265 = 0x68323635;
constexpr uint32_t kCodecAv1 = 0x00617631;
constexpr uint32_t kCodecOpus = 0x6f707573;
constexpr uint32_t kCodecAac = 0x00616163;
constexpr uint32_t kCodecFlac = 0x666c6163;
constexpr uint32_t kCodecRaw = 0x00726177;

std::once_flag wsa_once;

void ensure_winsock()
{
  std::call_once(wsa_once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
}

SOCKET as_socket(uintptr_t value)
{
  return static_cast<SOCKET>(value);
}

uintptr_t as_uintptr(SOCKET socket)
{
  return static_cast<uintptr_t>(socket);
}

void close_socket(uintptr_t &socket_value)
{
  if (socket_value != kInvalidSocket) {
    SOCKET socket = as_socket(socket_value);
    shutdown(socket, SD_BOTH);
    closesocket(socket);
    socket_value = kInvalidSocket;
  }
}

std::string quote_arg(const std::string &arg)
{
  if (arg.find_first_of(" \t\"") == std::string::npos) {
    return arg;
  }

  std::string out = "\"";
  for (char c : arg) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

std::string join_command(const std::vector<std::string> &args)
{
  std::ostringstream out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) {
      out << ' ';
    }
    out << quote_arg(args[i]);
  }
  return out.str();
}

std::string scrcpy_dir_from_path(const std::string &scrcpy_path)
{
  size_t slash = scrcpy_path.find_last_of("/\\");
  if (slash == std::string::npos) {
    return ".";
  }
  return scrcpy_path.substr(0, slash);
}

std::string to_windows_path(std::string path)
{
  std::replace(path.begin(), path.end(), '/', '\\');
  return path;
}

std::string adb_path(const ScrcpySettings &settings)
{
  std::string scrcpy_path =
      settings.scrcpy_path.empty() ? kDefaultScrcpyPath : settings.scrcpy_path;
  return to_windows_path(scrcpy_dir_from_path(scrcpy_path) + "\\adb.exe");
}

std::string server_path(const ScrcpySettings &settings)
{
  std::string scrcpy_path =
      settings.scrcpy_path.empty() ? kDefaultScrcpyPath : settings.scrcpy_path;
  return to_windows_path(scrcpy_dir_from_path(scrcpy_path) + "\\scrcpy-server");
}

std::vector<std::string> adb_selector(const ScrcpySettings &settings)
{
  std::vector<std::string> args;
  if (!settings.serial.empty()) {
    args.push_back("-s");
    args.push_back(settings.serial);
  } else if (settings.connection_mode == ConnectionMode::Tcpip &&
             !settings.tcpip_address.empty()) {
    args.push_back("-s");
    args.push_back(settings.tcpip_address);
  } else {
    args.push_back("-d");
  }
  return args;
}

bool run_process_wait(const std::vector<std::string> &args, DWORD *exit_code)
{
  std::string command = join_command(args);
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');

  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};

  BOOL ok = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!ok) {
    return false;
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (exit_code) {
    *exit_code = code;
  }
  return true;
}

bool start_process(const std::vector<std::string> &args, HANDLE *process)
{
  std::string command = join_command(args);
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');

  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};

  BOOL ok = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!ok) {
    return false;
  }

  CloseHandle(pi.hThread);
  *process = pi.hProcess;
  return true;
}

uint32_t read_be32(const uint8_t *data)
{
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
         (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

uint64_t read_be64(const uint8_t *data)
{
  uint64_t hi = read_be32(data);
  uint64_t lo = read_be32(data + 4);
  return (hi << 32) | lo;
}

bool recv_all(SOCKET socket, uint8_t *data, size_t len)
{
  size_t offset = 0;
  while (offset < len) {
    int r = recv(socket, reinterpret_cast<char *>(data + offset),
                 static_cast<int>(len - offset), 0);
    if (r <= 0) {
      return false;
    }
    offset += static_cast<size_t>(r);
  }
  return true;
}

bool wait_for_connection(SOCKET socket, int timeout_ms)
{
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(socket, &read_set);

  timeval timeout = {};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;

  return select(0, &read_set, nullptr, nullptr, &timeout) > 0;
}

uint32_t parse_bit_rate(const std::string &value, uint32_t fallback)
{
  if (value.empty()) {
    return fallback;
  }

  char *end = nullptr;
  double number = std::strtod(value.c_str(), &end);
  if (number <= 0) {
    return fallback;
  }

  uint64_t multiplier = 1;
  if (end && (*end == 'K' || *end == 'k')) {
    multiplier = 1000;
  } else if (end && (*end == 'M' || *end == 'm')) {
    multiplier = 1000000;
  }

  double result = number * static_cast<double>(multiplier);
  if (result > UINT32_MAX) {
    return fallback;
  }

  return static_cast<uint32_t>(result);
}

uint32_t codec_id_for_name(const std::string &name)
{
  if (name == "h264") {
    return kCodecH264;
  }
  if (name == "h265") {
    return kCodecH265;
  }
  if (name == "av1") {
    return kCodecAv1;
  }
  if (name == "opus") {
    return kCodecOpus;
  }
  if (name == "aac") {
    return kCodecAac;
  }
  if (name == "flac") {
    return kCodecFlac;
  }
  if (name == "raw") {
    return kCodecRaw;
  }
  return 0;
}

AVCodecID av_codec_for_scrcpy_id(uint32_t codec_id)
{
  switch (codec_id) {
  case kCodecH264:
    return AV_CODEC_ID_H264;
  case kCodecH265:
    return AV_CODEC_ID_HEVC;
  case kCodecAv1:
    return AV_CODEC_ID_AV1;
  case kCodecOpus:
    return AV_CODEC_ID_OPUS;
  case kCodecAac:
    return AV_CODEC_ID_AAC;
  case kCodecFlac:
    return AV_CODEC_ID_FLAC;
  case kCodecRaw:
    return AV_CODEC_ID_PCM_S16LE;
  default:
    return AV_CODEC_ID_NONE;
  }
}

enum video_colorspace obs_colorspace_for_frame(const AVFrame *frame)
{
  switch (frame->colorspace) {
  case AVCOL_SPC_BT470BG:
  case AVCOL_SPC_SMPTE170M:
  case AVCOL_SPC_SMPTE240M:
    return VIDEO_CS_601;
  case AVCOL_SPC_BT709:
    return VIDEO_CS_709;
  default:
    return frame->height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
  }
}

enum video_range_type obs_range_for_frame(const AVFrame *frame)
{
  return frame->color_range == AVCOL_RANGE_JPEG ? VIDEO_RANGE_FULL
                                                : VIDEO_RANGE_PARTIAL;
}

std::string av_error(int code)
{
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer = {};
  av_strerror(code, buffer.data(), buffer.size());
  return buffer.data();
}

void append_if_value(std::vector<std::string> &args, const char *key,
                     const std::string &value)
{
  if (!value.empty()) {
    args.push_back(std::string(key) + "=" + value);
  }
}

void write_le16(std::vector<uint8_t> &bytes, size_t offset, uint16_t value)
{
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void write_le32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
  bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
  bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint8_t clamp_color(int value)
{
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

std::vector<uint8_t> make_bridge_bmp(const AVFrame *frame, int max_width)
{
  const int source_width = frame->width;
  const int source_height = frame->height;
  const int target_width =
      std::min(source_width, std::max(320, max_width));
  const int target_height =
      std::max(1, source_height * target_width / source_width);
  const size_t pixel_bytes =
      static_cast<size_t>(target_width) * target_height * 4;
  std::vector<uint8_t> bmp(54 + pixel_bytes, 0);

  bmp[0] = 'B';
  bmp[1] = 'M';
  write_le32(bmp, 2, static_cast<uint32_t>(bmp.size()));
  write_le32(bmp, 10, 54);
  write_le32(bmp, 14, 40);
  write_le32(bmp, 18, static_cast<uint32_t>(target_width));
  write_le32(bmp, 22, static_cast<uint32_t>(-target_height));
  write_le16(bmp, 26, 1);
  write_le16(bmp, 28, 32);
  write_le32(bmp, 34, static_cast<uint32_t>(pixel_bytes));

  for (int y = 0; y < target_height; ++y) {
    const int source_y = y * source_height / target_height;
    for (int x = 0; x < target_width; ++x) {
      const int source_x = x * source_width / target_width;
      const int luma = frame->data[0][source_y * frame->linesize[0] + source_x];
      int u = 128;
      int v = 128;
      if (frame->format == AV_PIX_FMT_NV12) {
        const int chroma_offset =
            (source_y / 2) * frame->linesize[1] + (source_x / 2) * 2;
        u = frame->data[1][chroma_offset];
        v = frame->data[1][chroma_offset + 1];
      } else {
        u = frame->data[1][(source_y / 2) * frame->linesize[1] +
                           source_x / 2];
        v = frame->data[2][(source_y / 2) * frame->linesize[2] +
                           source_x / 2];
      }

      const int c = std::max(0, luma - 16);
      const int d = u - 128;
      const int e = v - 128;
      const uint8_t r = clamp_color((298 * c + 409 * e + 128) >> 8);
      const uint8_t g =
          clamp_color((298 * c - 100 * d - 208 * e + 128) >> 8);
      const uint8_t b = clamp_color((298 * c + 516 * d + 128) >> 8);
      const size_t target =
          54 + (static_cast<size_t>(y) * target_width + x) * 4;
      bmp[target] = b;
      bmp[target + 1] = g;
      bmp[target + 2] = r;
      bmp[target + 3] = 255;
    }
  }

  return bmp;
}

bool send_all(SOCKET socket, const uint8_t *data, size_t length)
{
  size_t sent = 0;
  while (sent < length) {
    const int chunk = send(socket, reinterpret_cast<const char *>(data + sent),
                           static_cast<int>(length - sent), 0);
    if (chunk <= 0) {
      return false;
    }
    sent += static_cast<size_t>(chunk);
  }
  return true;
}

bool post_bridge_bmp(const std::vector<uint8_t> &bmp)
{
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_SOCKET) {
    return false;
  }

  DWORD timeout_ms = 250;
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));

  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(8787);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (connect(socket, reinterpret_cast<sockaddr *>(&address),
              sizeof(address)) != 0) {
    closesocket(socket);
    return false;
  }

  std::ostringstream headers;
  headers << "POST /api/capture/obs/frame HTTP/1.1\r\n"
          << "Host: 127.0.0.1:8787\r\n"
          << "Content-Type: image/bmp\r\n"
          << "Content-Length: " << bmp.size() << "\r\n"
          << "X-MLBB-Source: obs-scrcpy-plugin\r\n"
          << "Connection: close\r\n\r\n";
  const std::string request_headers = headers.str();
  const bool ok =
      send_all(socket,
               reinterpret_cast<const uint8_t *>(request_headers.data()),
               request_headers.size()) &&
      send_all(socket, bmp.data(), bmp.size());
  shutdown(socket, SD_BOTH);
  closesocket(socket);
  return ok;
}

} // namespace

ScrcpyRuntime::ScrcpyRuntime(obs_source_t *source) : source_(source) {}

ScrcpyRuntime::~ScrcpyRuntime()
{
  stop();
}

void ScrcpyRuntime::start(const ScrcpySettings &settings)
{
  stop();
  settings_ = settings;
  stopping_ = false;
  last_bridge_frame_at_ = {};
  if (settings_.cv_bridge_enabled) {
    bridge_thread_ = std::thread(&ScrcpyRuntime::bridge_loop, this);
  }
  worker_ = std::thread(&ScrcpyRuntime::run, this);
}

void ScrcpyRuntime::stop()
{
  stopping_ = true;
  bridge_condition_.notify_all();
  close_socket(video_socket_);
  close_socket(audio_socket_);
  close_socket(listen_socket_);

  if (video_thread_.joinable()) {
    video_thread_.join();
  }
  if (audio_thread_.joinable()) {
    audio_thread_.join();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  if (bridge_thread_.joinable()) {
    bridge_thread_.join();
  }

  cleanup();
}

uint32_t ScrcpyRuntime::width() const
{
  return width_.load();
}

uint32_t ScrcpyRuntime::height() const
{
  return height_.load();
}

void ScrcpyRuntime::run()
{
  ensure_winsock();

  std::random_device rd;
  scid_ = rd();
  socket_name_ = "scrcpy_" + [&] {
    char buffer[9];
    snprintf(buffer, sizeof(buffer), "%08x", scid_);
    return std::string(buffer);
  }();

  if (!prepare_server_socket()) {
    blog(LOG_ERROR, "[obs-scrcpy-source] could not prepare local socket");
    return;
  }

  if (!run_adb_checked({adb_path(settings_), "start-server"}, "adb start-server")) {
    cleanup();
    return;
  }

  std::vector<std::string> selector = adb_selector(settings_);

  std::vector<std::string> push = {adb_path(settings_)};
  push.insert(push.end(), selector.begin(), selector.end());
  push.push_back("push");
  push.push_back(server_path(settings_));
  push.push_back("/data/local/tmp/scrcpy-server.jar");
  if (!run_adb_checked(push, "adb push scrcpy-server")) {
    cleanup();
    return;
  }

  std::vector<std::string> reverse = {adb_path(settings_)};
  reverse.insert(reverse.end(), selector.begin(), selector.end());
  reverse.push_back("reverse");
  reverse.push_back("localabstract:" + socket_name_);
  reverse.push_back("tcp:" + std::to_string(local_port_));
  if (!run_adb_checked(reverse, "adb reverse")) {
    cleanup();
    return;
  }

  if (!start_adb_server_process()) {
    blog(LOG_ERROR, "[obs-scrcpy-source] could not start scrcpy server");
    cleanup();
    return;
  }

  if (!accept_streams()) {
    cleanup();
    return;
  }

  video_thread_ = std::thread(&ScrcpyRuntime::video_loop, this);
  if (audio_socket_ != kInvalidSocket) {
    audio_thread_ = std::thread(&ScrcpyRuntime::audio_loop, this);
  }

  while (!stopping_) {
    if (server_process_) {
      DWORD wait = WaitForSingleObject(reinterpret_cast<HANDLE>(server_process_),
                                       100);
      if (wait == WAIT_OBJECT_0) {
        blog(LOG_INFO, "[obs-scrcpy-source] scrcpy server exited");
        break;
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void ScrcpyRuntime::cleanup()
{
  close_socket(video_socket_);
  close_socket(audio_socket_);
  close_socket(listen_socket_);

  if (server_process_) {
    HANDLE process = reinterpret_cast<HANDLE>(server_process_);
    DWORD code = 0;
    if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE) {
      TerminateProcess(process, 0);
      WaitForSingleObject(process, 1000);
    }
    CloseHandle(process);
    server_process_ = 0;
  }

  if (!socket_name_.empty() && local_port_) {
    std::vector<std::string> selector = adb_selector(settings_);
    std::vector<std::string> reverse_remove = {adb_path(settings_)};
    reverse_remove.insert(reverse_remove.end(), selector.begin(),
                          selector.end());
    reverse_remove.push_back("reverse");
    reverse_remove.push_back("--remove");
    reverse_remove.push_back("localabstract:" + socket_name_);
    DWORD exit_code = 0;
    run_process_wait(reverse_remove, &exit_code);
  }
}

bool ScrcpyRuntime::prepare_server_socket()
{
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_SOCKET) {
    return false;
  }

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (bind(socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    closesocket(socket);
    return false;
  }

  int len = sizeof(addr);
  if (getsockname(socket, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    closesocket(socket);
    return false;
  }

  if (listen(socket, 3) != 0) {
    closesocket(socket);
    return false;
  }

  local_port_ = ntohs(addr.sin_port);
  listen_socket_ = as_uintptr(socket);
  return true;
}

bool ScrcpyRuntime::run_adb_checked(const std::vector<std::string> &args,
                                    const char *label)
{
  DWORD exit_code = 1;
  bool ok = run_process_wait(args, &exit_code);
  if (!ok || exit_code != 0) {
    blog(LOG_ERROR, "[obs-scrcpy-source] %s failed (exit %lu): %s", label,
         static_cast<unsigned long>(exit_code), join_command(args).c_str());
    return false;
  }
  return true;
}

bool ScrcpyRuntime::start_adb_server_process()
{
  std::vector<std::string> selector = adb_selector(settings_);
  std::vector<std::string> args = {adb_path(settings_)};
  args.insert(args.end(), selector.begin(), selector.end());
  args.push_back("shell");
  args.push_back("CLASSPATH=/data/local/tmp/scrcpy-server.jar");
  args.push_back("app_process");
  args.push_back("/");
  args.push_back("com.genymobile.scrcpy.Server");
  args.push_back("4.0");

  char scid_text[16];
  snprintf(scid_text, sizeof(scid_text), "scid=%08x", scid_);
  args.push_back(scid_text);
  args.push_back("log_level=info");
  args.push_back("control=false");
  args.push_back("cleanup=true");
  args.push_back("video=true");
  args.push_back("audio=true");
  args.push_back("video_codec=" + settings_.video_codec);
  args.push_back("audio_codec=" + settings_.audio_codec);
  args.push_back("video_bit_rate=" +
                 std::to_string(parse_bit_rate(settings_.video_bit_rate,
                                                8000000)));
  args.push_back("audio_bit_rate=" +
                 std::to_string(parse_bit_rate(settings_.audio_bit_rate,
                                                128000)));
  append_if_value(args, "video_encoder", settings_.video_encoder);
  append_if_value(args, "audio_encoder", settings_.audio_encoder);
  append_if_value(args, "video_codec_options", settings_.video_codec_options);
  append_if_value(args, "audio_codec_options", settings_.audio_codec_options);
  if (settings_.max_size > 0) {
    args.push_back("max_size=" + std::to_string(settings_.max_size));
  }
  if (settings_.max_fps > 0) {
    args.push_back("max_fps=" + std::to_string(settings_.max_fps));
  }
  if (!settings_.audio_source.empty() && settings_.audio_source != "output") {
    args.push_back("audio_source=" + settings_.audio_source);
  }

  HANDLE process = nullptr;
  bool ok = start_process(args, &process);
  if (ok) {
    server_process_ = reinterpret_cast<uintptr_t>(process);
  }
  return ok;
}

bool ScrcpyRuntime::accept_streams()
{
  SOCKET listen_socket = as_socket(listen_socket_);
  if (!wait_for_connection(listen_socket, 10000)) {
    blog(LOG_ERROR,
         "[obs-scrcpy-source] timed out waiting for video socket");
    return false;
  }

  SOCKET video_socket = accept(listen_socket, nullptr, nullptr);
  if (video_socket == INVALID_SOCKET) {
    blog(LOG_ERROR, "[obs-scrcpy-source] could not accept video socket");
    return false;
  }
  video_socket_ = as_uintptr(video_socket);

  std::array<uint8_t, kDeviceNameFieldLength> name_buffer = {};
  if (!recv_all(video_socket, name_buffer.data(), name_buffer.size())) {
    blog(LOG_ERROR, "[obs-scrcpy-source] missing device metadata");
    return false;
  }
  device_name_ = reinterpret_cast<const char *>(name_buffer.data());
  blog(LOG_INFO, "[obs-scrcpy-source] connected to %s", device_name_.c_str());

  if (!wait_for_connection(listen_socket, 3000)) {
    blog(LOG_WARNING, "[obs-scrcpy-source] audio socket unavailable");
    return true;
  }

  SOCKET audio_socket = accept(listen_socket, nullptr, nullptr);
  if (audio_socket != INVALID_SOCKET) {
    audio_socket_ = as_uintptr(audio_socket);
  } else {
    blog(LOG_WARNING, "[obs-scrcpy-source] could not accept audio socket");
  }

  return true;
}

void ScrcpyRuntime::video_loop()
{
  SOCKET socket = as_socket(video_socket_);

  uint8_t codec_buf[4] = {};
  if (!recv_all(socket, codec_buf, 4)) {
    return;
  }

  uint32_t raw_codec_id = read_be32(codec_buf);
  AVCodecID codec_id = av_codec_for_scrcpy_id(raw_codec_id);
  const AVCodec *codec = avcodec_find_decoder(codec_id);
  if (!codec) {
    blog(LOG_ERROR, "[obs-scrcpy-source] missing video decoder for codec 0x%08x",
         raw_codec_id);
    return;
  }

  std::array<uint8_t, kPacketHeaderSize> header = {};
  if (!recv_all(socket, header.data(), header.size()) ||
      !(read_be64(header.data()) & kPacketFlagSession)) {
    blog(LOG_ERROR, "[obs-scrcpy-source] missing video session packet");
    return;
  }

  uint32_t session_width = read_be32(header.data() + 4);
  uint32_t session_height = read_be32(header.data() + 8);
  width_ = session_width;
  height_ = session_height;

  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    return;
  }
  ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  ctx->width = static_cast<int>(session_width);
  ctx->height = static_cast<int>(session_height);
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;

  int ret = avcodec_open2(ctx, codec, nullptr);
  if (ret < 0) {
    blog(LOG_ERROR, "[obs-scrcpy-source] could not open video decoder: %s",
         av_error(ret).c_str());
    avcodec_free_context(&ctx);
    return;
  }

  AVFrame *frame = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  std::vector<uint8_t> config;
  bool logged_first_frame = false;
  bool logged_unsupported_format = false;

  while (!stopping_) {
    if (!recv_all(socket, header.data(), header.size())) {
      break;
    }

    uint64_t pts_flags = read_be64(header.data());
    if (pts_flags & kPacketFlagSession) {
      width_ = read_be32(header.data() + 4);
      height_ = read_be32(header.data() + 8);
      continue;
    }

    uint32_t packet_size = read_be32(header.data() + 8);
    if (!packet_size || av_new_packet(packet, packet_size) < 0) {
      break;
    }

    if (!recv_all(socket, packet->data, packet_size)) {
      av_packet_unref(packet);
      break;
    }

    bool is_config = (pts_flags & kPacketFlagConfig) != 0;
    if (is_config && (raw_codec_id == kCodecH264 || raw_codec_id == kCodecH265)) {
      config.assign(packet->data, packet->data + packet->size);
      av_packet_unref(packet);
      continue;
    }

    if (!config.empty() &&
        (raw_codec_id == kCodecH264 || raw_codec_id == kCodecH265)) {
      int media_size = packet->size;
      av_grow_packet(packet, static_cast<int>(config.size()));
      memmove(packet->data + config.size(), packet->data, media_size);
      memcpy(packet->data, config.data(), config.size());
      config.clear();
    }

    packet->pts = is_config ? AV_NOPTS_VALUE
                            : static_cast<int64_t>(pts_flags & kPacketPtsMask);
    packet->dts = packet->pts;
    if (pts_flags & kPacketFlagKeyFrame) {
      packet->flags |= AV_PKT_FLAG_KEY;
    }

    if (!is_config) {
      ret = avcodec_send_packet(ctx, packet);
      if (ret >= 0 || ret == AVERROR(EAGAIN)) {
        for (;;) {
          ret = avcodec_receive_frame(ctx, frame);
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
          }
          if (ret < 0) {
            blog(LOG_ERROR,
                 "[obs-scrcpy-source] could not receive video frame: %s",
                 av_error(ret).c_str());
            break;
          }

          if (frame->format == AV_PIX_FMT_YUV420P ||
              frame->format == AV_PIX_FMT_NV12) {
            obs_source_frame obs_frame = {};
            obs_frame.format = frame->format == AV_PIX_FMT_NV12
                                   ? VIDEO_FORMAT_NV12
                                   : VIDEO_FORMAT_I420;
            obs_frame.width = static_cast<uint32_t>(frame->width);
            obs_frame.height = static_cast<uint32_t>(frame->height);
            obs_frame.timestamp =
                os_gettime_ns();
            obs_frame.full_range =
                obs_range_for_frame(frame) == VIDEO_RANGE_FULL;
            video_format_get_parameters_for_format(
                obs_colorspace_for_frame(frame), obs_range_for_frame(frame),
                obs_frame.format, obs_frame.color_matrix,
                obs_frame.color_range_min, obs_frame.color_range_max);
            obs_frame.data[0] = frame->data[0];
            obs_frame.data[1] = frame->data[1];
            obs_frame.linesize[0] = static_cast<uint32_t>(frame->linesize[0]);
            obs_frame.linesize[1] = static_cast<uint32_t>(frame->linesize[1]);
            if (frame->format == AV_PIX_FMT_YUV420P) {
              obs_frame.data[2] = frame->data[2];
              obs_frame.linesize[2] =
                  static_cast<uint32_t>(frame->linesize[2]);
            }
            obs_source_output_video(source_, &obs_frame);
            queue_bridge_frame(frame);
            width_ = obs_frame.width;
            height_ = obs_frame.height;
            if (!logged_first_frame) {
              const char *format_name =
                  av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
              blog(LOG_INFO,
                   "[obs-scrcpy-source] output video frame %ux%u %s",
                   obs_frame.width, obs_frame.height,
                   format_name ? format_name : "unknown");
              logged_first_frame = true;
            }
          } else if (!logged_unsupported_format) {
            const char *format_name =
                av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
            blog(LOG_WARNING,
                 "[obs-scrcpy-source] unsupported decoded video format %s",
                 format_name ? format_name : "unknown");
            logged_unsupported_format = true;
          }

          av_frame_unref(frame);
        }
      }
    }

    av_packet_unref(packet);
  }

  obs_source_output_video(source_, nullptr);
  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&ctx);
}

void ScrcpyRuntime::queue_bridge_frame(const AVFrame *frame)
{
  if (!settings_.cv_bridge_enabled) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const int fps = std::clamp(settings_.cv_bridge_fps, 1, 15);
  const auto interval = std::chrono::milliseconds(1000 / fps);
  if (last_bridge_frame_at_.time_since_epoch().count() &&
      now - last_bridge_frame_at_ < interval) {
    return;
  }
  last_bridge_frame_at_ = now;

  std::vector<uint8_t> bmp =
      make_bridge_bmp(frame, settings_.cv_bridge_max_width);
  {
    std::lock_guard<std::mutex> lock(bridge_mutex_);
    bridge_pending_frame_ = std::move(bmp);
  }
  bridge_condition_.notify_one();
}

void ScrcpyRuntime::bridge_loop()
{
  while (!stopping_) {
    std::vector<uint8_t> frame;
    {
      std::unique_lock<std::mutex> lock(bridge_mutex_);
      bridge_condition_.wait(lock, [this] {
        return stopping_ || !bridge_pending_frame_.empty();
      });
      if (stopping_) {
        break;
      }
      frame.swap(bridge_pending_frame_);
    }

    if (!frame.empty()) {
      post_bridge_bmp(frame);
    }
  }
}

void ScrcpyRuntime::audio_loop()
{
  SOCKET socket = as_socket(audio_socket_);

  uint8_t codec_buf[4] = {};
  if (!recv_all(socket, codec_buf, 4)) {
    return;
  }

  uint32_t raw_codec_id = read_be32(codec_buf);
  AVCodecID codec_id = av_codec_for_scrcpy_id(raw_codec_id);
  const AVCodec *codec = avcodec_find_decoder(codec_id);
  if (!codec) {
    blog(LOG_WARNING, "[obs-scrcpy-source] missing audio decoder for codec 0x%08x",
         raw_codec_id);
    return;
  }

  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    return;
  }
  ctx->sample_rate = 48000;
  av_channel_layout_default(&ctx->ch_layout, 2);
  if (raw_codec_id == kCodecFlac || raw_codec_id == kCodecRaw) {
    ctx->sample_fmt = AV_SAMPLE_FMT_S16;
  }

  int ret = avcodec_open2(ctx, codec, nullptr);
  if (ret < 0) {
    blog(LOG_WARNING, "[obs-scrcpy-source] could not open audio decoder: %s",
         av_error(ret).c_str());
    avcodec_free_context(&ctx);
    return;
  }

  AVFrame *frame = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  SwrContext *swr = nullptr;
  AVChannelLayout stereo;
  av_channel_layout_default(&stereo, 2);
  swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000,
                      &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, 0,
                      nullptr);
  if (swr) {
    swr_init(swr);
  }

  std::vector<uint8_t> converted;
  std::array<uint8_t, kPacketHeaderSize> header = {};

  while (!stopping_) {
    if (!recv_all(socket, header.data(), header.size())) {
      break;
    }

    uint64_t pts_flags = read_be64(header.data());
    uint32_t packet_size = read_be32(header.data() + 8);
    if (!packet_size || av_new_packet(packet, packet_size) < 0) {
      break;
    }

    if (!recv_all(socket, packet->data, packet_size)) {
      av_packet_unref(packet);
      break;
    }

    bool is_config = (pts_flags & kPacketFlagConfig) != 0;
    if (!is_config) {
      packet->pts = static_cast<int64_t>(pts_flags & kPacketPtsMask);
      packet->dts = packet->pts;
      ret = avcodec_send_packet(ctx, packet);
      if (ret >= 0 || ret == AVERROR(EAGAIN)) {
        for (;;) {
          ret = avcodec_receive_frame(ctx, frame);
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
          }
          if (ret < 0) {
            break;
          }

          if (swr) {
            int out_samples =
                swr_get_out_samples(swr, frame->nb_samples);
            converted.resize(static_cast<size_t>(out_samples) * 2 * sizeof(int16_t));
            uint8_t *out_data[1] = {converted.data()};
            int samples = swr_convert(swr, out_data, out_samples,
                                      const_cast<const uint8_t **>(frame->extended_data),
                                      frame->nb_samples);
            if (samples > 0) {
              obs_source_audio audio = {};
              audio.data[0] = converted.data();
              audio.frames = static_cast<uint32_t>(samples);
              audio.speakers = SPEAKERS_STEREO;
              audio.format = AUDIO_FORMAT_16BIT;
              audio.samples_per_sec = 48000;
              audio.timestamp =
                  os_gettime_ns() + uint64_t(settings_.audio_buffer_ms) * 1000000;
              obs_source_output_audio(source_, &audio);
            }
          }

          av_frame_unref(frame);
        }
      }
    }

    av_packet_unref(packet);
  }

  if (swr) {
    swr_free(&swr);
  }
  av_channel_layout_uninit(&stereo);
  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&ctx);
}

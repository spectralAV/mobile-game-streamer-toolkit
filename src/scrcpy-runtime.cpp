#include "scrcpy-runtime.h"

#include "scrcpy-command.h"
#include "scrcpy-low-latency.h"

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
#include <libavutil/hwcontext.h>
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
constexpr uint32_t kCodecVp8 = 0x00767038;
constexpr uint32_t kCodecVp9 = 0x00767039;
constexpr uint32_t kCodecOpus = 0x6f707573;
constexpr uint32_t kCodecAac = 0x00616163;
constexpr uint32_t kCodecFlac = 0x666c6163;
constexpr uint32_t kCodecRaw = 0x00726177;
constexpr int kCvBridgeRealtimeMaxFps = 10;
constexpr int kCvBridgeMaxWidth = 4096;

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

std::string detect_scrcpy_server_version(const ScrcpySettings &settings)
{
  const std::string scrcpy_path =
      settings.scrcpy_path.empty() ? kDefaultScrcpyPath : settings.scrcpy_path;
  const std::string command = quote_arg(scrcpy_path) + " --version 2>&1";
  FILE *pipe = _popen(command.c_str(), "r");
  if (!pipe) {
    return {};
  }

  std::array<char, 1024> buffer = {};
  std::string output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
    output += buffer.data();
  }
  const int exit_code = _pclose(pipe);
  if (exit_code != 0) {
    return {};
  }
  return parse_scrcpy_version_output(output);
}

bool run_process_wait(const std::vector<std::string> &args, DWORD *exit_code,
                      DWORD timeout_ms = 10000)
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

  const DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
  DWORD code = 1;
  if (wait == WAIT_TIMEOUT) {
    TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
    WaitForSingleObject(pi.hProcess, 1000);
    code = ERROR_TIMEOUT;
  } else {
    GetExitCodeProcess(pi.hProcess, &code);
  }
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

bool discard_socket_data(SOCKET socket, size_t len)
{
  std::array<uint8_t, 65536> scratch = {};
  size_t remaining = len;
  while (remaining > 0) {
    const size_t chunk = std::min(remaining, scratch.size());
    const int r = recv(socket, reinterpret_cast<char *>(scratch.data()),
                       static_cast<int>(chunk), 0);
    if (r <= 0) {
      return false;
    }
    remaining -= static_cast<size_t>(r);
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
  if (name == "vp8") {
    return kCodecVp8;
  }
  if (name == "vp9") {
    return kCodecVp9;
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
  case kCodecVp8:
    return AV_CODEC_ID_VP8;
  case kCodecVp9:
    return AV_CODEC_ID_VP9;
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

const AVCodec *find_video_decoder(uint32_t codec_id)
{
  if (codec_id == kCodecAv1) {
    // OBS's FFmpeg registers libaom first for AV1. Prefer dav1d when bundled,
    // then the native decoder, which can expose D3D11VA and is substantially
    // faster than libaom for this realtime decode path.
    if (const AVCodec *codec = avcodec_find_decoder_by_name("libdav1d")) {
      return codec;
    }
    if (const AVCodec *codec = avcodec_find_decoder_by_name("av1")) {
      return codec;
    }
  } else if (codec_id == kCodecVp8) {
    if (const AVCodec *codec = avcodec_find_decoder_by_name("vp8")) {
      return codec;
    }
  } else if (codec_id == kCodecVp9) {
    if (const AVCodec *codec = avcodec_find_decoder_by_name("vp9")) {
      return codec;
    }
  }
  return avcodec_find_decoder(av_codec_for_scrcpy_id(codec_id));
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

bool obs_format_for_frame(const AVFrame *frame, enum video_format &format)
{
  switch (frame->format) {
  case AV_PIX_FMT_YUV420P:
    format = VIDEO_FORMAT_I420;
    return true;
  case AV_PIX_FMT_NV12:
    format = VIDEO_FORMAT_NV12;
    return true;
  default:
    return false;
  }
}

bool frame_uses_three_planes(const AVFrame *frame)
{
  return frame->format == AV_PIX_FMT_YUV420P ||
         frame->format == AV_PIX_FMT_YUV420P10LE;
}

void fill_obs_frame_metadata(obs_source_frame &obs_frame, const AVFrame *frame,
                             enum video_format format, uint64_t timestamp_ns)
{
  obs_frame.format = format;
  obs_frame.width = static_cast<uint32_t>(frame->width);
  obs_frame.height = static_cast<uint32_t>(frame->height);
  obs_frame.timestamp = timestamp_ns;
  obs_frame.full_range = obs_range_for_frame(frame) == VIDEO_RANGE_FULL;
  video_format_get_parameters_for_format(
      obs_colorspace_for_frame(frame), obs_range_for_frame(frame),
      obs_frame.format, obs_frame.color_matrix, obs_frame.color_range_min,
      obs_frame.color_range_max);
}

void output_direct_obs_frame(obs_source_t *source, const AVFrame *frame,
                             enum video_format format, uint64_t timestamp_ns)
{
  obs_source_frame obs_frame = {};
  fill_obs_frame_metadata(obs_frame, frame, format, timestamp_ns);
  obs_frame.data[0] = frame->data[0];
  obs_frame.data[1] = frame->data[1];
  obs_frame.linesize[0] = static_cast<uint32_t>(frame->linesize[0]);
  obs_frame.linesize[1] = static_cast<uint32_t>(frame->linesize[1]);
  if (frame_uses_three_planes(frame)) {
    obs_frame.data[2] = frame->data[2];
    obs_frame.linesize[2] = static_cast<uint32_t>(frame->linesize[2]);
  }
  obs_source_output_video(source, &obs_frame);
}

bool output_downsampled_10bit_frame(obs_source_t *source, const AVFrame *frame,
                                    uint64_t timestamp_ns)
{
  if (frame->format != AV_PIX_FMT_YUV420P10LE &&
      frame->format != AV_PIX_FMT_P010LE) {
    return false;
  }

  const int width = frame->width;
  const int height = frame->height;
  const int chroma_width = (width + 1) / 2;
  const int chroma_height = (height + 1) / 2;
  static thread_local std::vector<uint8_t> y;
  static thread_local std::vector<uint8_t> u;
  static thread_local std::vector<uint8_t> v;
  y.resize(static_cast<size_t>(width) * height);
  u.resize(static_cast<size_t>(chroma_width) * chroma_height);
  v.resize(static_cast<size_t>(chroma_width) * chroma_height);

  for (int row = 0; row < height; ++row) {
    const uint8_t *src = frame->data[0] + row * frame->linesize[0];
    const uint16_t *samples = reinterpret_cast<const uint16_t *>(src);
    uint8_t *dst = y.data() + static_cast<size_t>(row) * width;
    for (int col = 0; col < width; ++col) {
      const uint16_t sample = samples[col];
      dst[col] = frame->format == AV_PIX_FMT_P010LE
                     ? static_cast<uint8_t>(sample >> 8)
                     : static_cast<uint8_t>(std::min<int>(255, sample >> 2));
    }
  }

  for (int row = 0; row < chroma_height; ++row) {
    uint8_t *u_dst = u.data() + static_cast<size_t>(row) * chroma_width;
    uint8_t *v_dst = v.data() + static_cast<size_t>(row) * chroma_width;
    if (frame->format == AV_PIX_FMT_P010LE) {
      const uint8_t *src = frame->data[1] + row * frame->linesize[1];
      const uint16_t *samples = reinterpret_cast<const uint16_t *>(src);
      for (int col = 0; col < chroma_width; ++col) {
        u_dst[col] = static_cast<uint8_t>(samples[col * 2] >> 8);
        v_dst[col] = static_cast<uint8_t>(samples[col * 2 + 1] >> 8);
      }
    } else {
      const uint8_t *u_src = frame->data[1] + row * frame->linesize[1];
      const uint8_t *v_src = frame->data[2] + row * frame->linesize[2];
      const uint16_t *u_samples = reinterpret_cast<const uint16_t *>(u_src);
      const uint16_t *v_samples = reinterpret_cast<const uint16_t *>(v_src);
      for (int col = 0; col < chroma_width; ++col) {
        u_dst[col] =
            static_cast<uint8_t>(std::min<int>(255, u_samples[col] >> 2));
        v_dst[col] =
            static_cast<uint8_t>(std::min<int>(255, v_samples[col] >> 2));
      }
    }
  }

  obs_source_frame obs_frame = {};
  fill_obs_frame_metadata(obs_frame, frame, VIDEO_FORMAT_I420, timestamp_ns);
  obs_frame.data[0] = y.data();
  obs_frame.data[1] = u.data();
  obs_frame.data[2] = v.data();
  obs_frame.linesize[0] = static_cast<uint32_t>(width);
  obs_frame.linesize[1] = static_cast<uint32_t>(chroma_width);
  obs_frame.linesize[2] = static_cast<uint32_t>(chroma_width);
  obs_source_output_video(source, &obs_frame);
  return true;
}

bool output_obs_video_frame(obs_source_t *source, const AVFrame *frame,
                            uint64_t timestamp_ns)
{
  enum video_format obs_format = VIDEO_FORMAT_NONE;
  if (obs_format_for_frame(frame, obs_format)) {
    output_direct_obs_frame(source, frame, obs_format, timestamp_ns);
    return true;
  }
  return output_downsampled_10bit_frame(source, frame, timestamp_ns);
}

uint8_t read_luma8(const AVFrame *frame, int x, int y)
{
  const uint8_t *row = frame->data[0] + y * frame->linesize[0];
  if (frame->format == AV_PIX_FMT_YUV420P10LE) {
    const uint16_t sample = reinterpret_cast<const uint16_t *>(row)[x];
    return static_cast<uint8_t>(std::min<int>(255, sample >> 2));
  }
  if (frame->format == AV_PIX_FMT_P010LE) {
    const uint16_t sample = reinterpret_cast<const uint16_t *>(row)[x];
    return static_cast<uint8_t>(sample >> 8);
  }
  return row[x];
}

void read_chroma8(const AVFrame *frame, int x, int y, int &u, int &v)
{
  if (frame->format == AV_PIX_FMT_NV12) {
    const int chroma_offset = (y / 2) * frame->linesize[1] + (x / 2) * 2;
    u = frame->data[1][chroma_offset];
    v = frame->data[1][chroma_offset + 1];
    return;
  }
  if (frame->format == AV_PIX_FMT_P010LE) {
    const uint8_t *row = frame->data[1] + (y / 2) * frame->linesize[1];
    const uint16_t *samples = reinterpret_cast<const uint16_t *>(row);
    u = static_cast<uint8_t>(samples[(x / 2) * 2] >> 8);
    v = static_cast<uint8_t>(samples[(x / 2) * 2 + 1] >> 8);
    return;
  }
  if (frame->format == AV_PIX_FMT_YUV420P10LE) {
    const uint8_t *u_row = frame->data[1] + (y / 2) * frame->linesize[1];
    const uint8_t *v_row = frame->data[2] + (y / 2) * frame->linesize[2];
    u = static_cast<uint8_t>(
        std::min<int>(255, reinterpret_cast<const uint16_t *>(u_row)[x / 2] >> 2));
    v = static_cast<uint8_t>(
        std::min<int>(255, reinterpret_cast<const uint16_t *>(v_row)[x / 2] >> 2));
    return;
  }
  u = frame->data[1][(y / 2) * frame->linesize[1] + x / 2];
  v = frame->data[2][(y / 2) * frame->linesize[2] + x / 2];
}

std::string av_error(int code)
{
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer = {};
  av_strerror(code, buffer.data(), buffer.size());
  return buffer.data();
}

AVPixelFormat hardware_pixel_format(const AVCodec *codec,
                                    AVHWDeviceType device_type)
{
  for (int index = 0;; ++index) {
    const AVCodecHWConfig *config = avcodec_get_hw_config(codec, index);
    if (!config) {
      return AV_PIX_FMT_NONE;
    }
    if (config->device_type == device_type &&
        (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
      return config->pix_fmt;
    }
  }
}

AVPixelFormat select_hardware_pixel_format(AVCodecContext *ctx,
                                           const AVPixelFormat *formats)
{
  const auto *desired = static_cast<const AVPixelFormat *>(ctx->opaque);
  if (desired) {
    for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE;
         ++format) {
      if (*format == *desired) {
        return *format;
      }
    }
  }
  return avcodec_default_get_format(ctx, formats);
}

void configure_software_decoder(AVCodecContext *ctx, uint32_t width,
                                uint32_t height)
{
  ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  ctx->flags2 |= AV_CODEC_FLAG2_FAST;
  ctx->thread_count = 0;
  ctx->thread_type = FF_THREAD_SLICE;
  ctx->width = static_cast<int>(width);
  ctx->height = static_cast<int>(height);
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
}

bool configure_d3d11va_decoder(AVCodecContext *ctx, const AVCodec *codec,
                               AVPixelFormat &hardware_format)
{
  hardware_format = hardware_pixel_format(codec, AV_HWDEVICE_TYPE_D3D11VA);
  if (hardware_format == AV_PIX_FMT_NONE) {
    return false;
  }

  const int ret = av_hwdevice_ctx_create(&ctx->hw_device_ctx,
                                         AV_HWDEVICE_TYPE_D3D11VA, nullptr,
                                         nullptr, 0);
  if (ret < 0) {
    blog(LOG_WARNING,
         "[obs-scrcpy-source] D3D11VA device unavailable, using software decode: %s",
         av_error(ret).c_str());
    hardware_format = AV_PIX_FMT_NONE;
    return false;
  }

  ctx->opaque = &hardware_format;
  ctx->get_format = select_hardware_pixel_format;
  return true;
}

void append_if_value(std::vector<std::string> &args, const char *key,
                     const std::string &value)
{
  if (!value.empty()) {
    args.push_back(std::string(key) + "=" + value);
  }
}

uint8_t clamp_color(int value)
{
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

std::vector<uint8_t> make_bridge_bgra(const AVFrame *frame, int max_width,
                                      int &target_width, int &target_height)
{
  const int source_width = frame->width;
  const int source_height = frame->height;
  target_width = max_width <= 0
                     ? source_width
                     : std::min(source_width, std::max(320, max_width));
  target_height =
      std::max(1, source_height * target_width / source_width);
  const size_t pixel_bytes =
      static_cast<size_t>(target_width) * target_height * 4;
  std::vector<uint8_t> bgra(pixel_bytes, 0);

  for (int y = 0; y < target_height; ++y) {
    const int source_y = y * source_height / target_height;
    for (int x = 0; x < target_width; ++x) {
      const int source_x = x * source_width / target_width;
      const int luma = read_luma8(frame, source_x, source_y);
      int u = 128;
      int v = 128;
      read_chroma8(frame, source_x, source_y, u, v);

      const int c = std::max(0, luma - 16);
      const int d = u - 128;
      const int e = v - 128;
      const uint8_t r = clamp_color((298 * c + 409 * e + 128) >> 8);
      const uint8_t g =
          clamp_color((298 * c - 100 * d - 208 * e + 128) >> 8);
      const uint8_t b = clamp_color((298 * c + 516 * d + 128) >> 8);
      const size_t target = (static_cast<size_t>(y) * target_width + x) * 4;
      bgra[target] = b;
      bgra[target + 1] = g;
      bgra[target + 2] = r;
      bgra[target + 3] = 255;
    }
  }

  return bgra;
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

bool post_bridge_raw_frame(const std::vector<uint8_t> &frame, int width,
                           int height)
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
  headers << "POST /api/capture/obs/frame.raw HTTP/1.1\r\n"
          << "Host: 127.0.0.1:8787\r\n"
          << "Content-Type: application/vnd.mlbb.raw-frame\r\n"
          << "Content-Length: " << frame.size() << "\r\n"
          << "X-MLBB-Source: obs-scrcpy-plugin\r\n"
          << "X-Source-Width: " << width << "\r\n"
          << "X-Source-Height: " << height << "\r\n"
          << "X-Pixel-Format: BGRA\r\n"
          << "Connection: close\r\n\r\n";
  const std::string request_headers = headers.str();
  const bool ok =
      send_all(socket,
               reinterpret_cast<const uint8_t *>(request_headers.data()),
               request_headers.size()) &&
      send_all(socket, frame.data(), frame.size());
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
  server_version_.clear();
  stopping_ = false;
  video_stream_ended_ = false;
  bridge_busy_ = false;
  last_bridge_frame_at_ = {};
  latest_video_pts_us_ = 0;
  latest_presented_video_pts_us_ = 0;
  if (settings_.cv_bridge_enabled) {
    bridge_thread_ = std::thread(&ScrcpyRuntime::bridge_loop, this);
    SetThreadPriority(static_cast<HANDLE>(bridge_thread_.native_handle()),
                      THREAD_PRIORITY_BELOW_NORMAL);
  }
  worker_ = std::thread(&ScrcpyRuntime::run, this);
}

void ScrcpyRuntime::stop()
{
  stopping_ = true;
  bridge_condition_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }
  if (video_thread_.joinable()) {
    video_thread_.join();
  }
  if (audio_thread_.joinable()) {
    audio_thread_.join();
  }
  if (bridge_thread_.joinable()) {
    bridge_thread_.join();
  }

  AVFrame *pending_bridge_frame = nullptr;
  {
    std::lock_guard<std::mutex> lock(bridge_mutex_);
    pending_bridge_frame = bridge_pending_frame_;
    bridge_pending_frame_ = nullptr;
  }
  if (pending_bridge_frame) {
    av_frame_free(&pending_bridge_frame);
  }
  bridge_busy_ = false;

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

  unsigned int retry_count = 0;
  while (!stopping_) {
    const bool connected = start_session();
    if (connected) {
      retry_count = 0;
      const uint64_t video_session_started_ns = os_gettime_ns();
      while (!stopping_) {
        if (video_stream_ended_) {
          blog(LOG_WARNING,
               "[obs-scrcpy-source] video stream ended; reconnecting automatically");
          break;
        }

        if (scrcpy_first_video_frame_timed_out(
                os_gettime_ns(), video_session_started_ns,
                first_video_frame_at_ns_.load())) {
          blog(LOG_WARNING,
               "[obs-scrcpy-source] no first video frame within %u ms; reconnecting automatically",
               kFirstVideoFrameTimeoutMs);
          break;
        }

        if (server_process_) {
          DWORD wait =
              WaitForSingleObject(reinterpret_cast<HANDLE>(server_process_),
                                  100);
          if (wait == WAIT_OBJECT_0) {
            blog(LOG_INFO,
                 "[obs-scrcpy-source] scrcpy server exited; reconnecting automatically");
            break;
          }
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    }

    stop_session();
    if (stopping_) {
      break;
    }

    ++retry_count;
    const auto delay = std::chrono::milliseconds(
        scrcpy_reconnect_delay_ms(retry_count));
    blog(LOG_INFO,
         "[obs-scrcpy-source] reconnect attempt %u scheduled in %lld ms",
         retry_count, static_cast<long long>(delay.count()));
    if (!wait_before_retry(delay)) {
      break;
    }
  }
}

bool ScrcpyRuntime::start_session()
{
  if (server_version_.empty()) {
    server_version_ = detect_scrcpy_server_version(settings_);
  }
  if (!is_supported_scrcpy_server_version(server_version_)) {
    if (server_version_.empty()) {
      const std::string scrcpy_path = settings_.scrcpy_path.empty()
                                          ? kDefaultScrcpyPath
                                          : settings_.scrcpy_path;
      blog(LOG_ERROR,
           "[obs-scrcpy-source] could not detect the scrcpy version from %s",
           scrcpy_path.c_str());
    } else {
      blog(LOG_ERROR,
           "[obs-scrcpy-source] unsupported scrcpy server version %s; supported versions are 4.0 and 4.1",
           server_version_.c_str());
    }
    return false;
  }

  std::random_device rd;
  scid_ = normalize_scrcpy_scid(rd());
  socket_name_ = "scrcpy_" + [&] {
    char buffer[9];
    snprintf(buffer, sizeof(buffer), "%08x", scid_);
    return std::string(buffer);
  }();

  if (!prepare_server_socket()) {
    blog(LOG_ERROR, "[obs-scrcpy-source] could not prepare local socket");
    return false;
  }

  if (!run_adb_checked({adb_path(settings_), "start-server"}, "adb start-server")) {
    return false;
  }

  const std::vector<std::string> connect_args =
      build_adb_connect_args(settings_);
  if (!connect_args.empty()) {
    std::vector<std::string> connect = {adb_path(settings_)};
    connect.insert(connect.end(), connect_args.begin(), connect_args.end());
    if (!run_adb_checked(connect, "adb connect TCP/IP device")) {
      return false;
    }
  }

  std::vector<std::string> selector = build_adb_selector_args(settings_);
  std::vector<std::string> get_state = {adb_path(settings_)};
  get_state.insert(get_state.end(), selector.begin(), selector.end());
  get_state.push_back("get-state");
  if (!run_adb_checked(get_state, "adb verify selected device")) {
    return false;
  }

  std::vector<std::string> push = {adb_path(settings_)};
  push.insert(push.end(), selector.begin(), selector.end());
  push.push_back("push");
  push.push_back(server_path(settings_));
  push.push_back("/data/local/tmp/scrcpy-server.jar");
  if (!run_adb_checked(push, "adb push scrcpy-server")) {
    return false;
  }

  std::vector<std::string> reverse = {adb_path(settings_)};
  reverse.insert(reverse.end(), selector.begin(), selector.end());
  reverse.push_back("reverse");
  reverse.push_back("localabstract:" + socket_name_);
  reverse.push_back("tcp:" + std::to_string(local_port_));
  if (!run_adb_checked(reverse, "adb reverse")) {
    return false;
  }

  if (!start_adb_server_process()) {
    blog(LOG_ERROR, "[obs-scrcpy-source] could not start scrcpy server");
    return false;
  }

  if (!accept_streams()) {
    return false;
  }

  video_stream_ended_ = false;
  first_video_frame_at_ns_ = 0;
  video_thread_ = std::thread(&ScrcpyRuntime::video_loop, this);
  SetThreadPriority(static_cast<HANDLE>(video_thread_.native_handle()),
                    THREAD_PRIORITY_ABOVE_NORMAL);
  if (audio_socket_ != kInvalidSocket) {
    audio_thread_ = std::thread(&ScrcpyRuntime::audio_loop, this);
    SetThreadPriority(static_cast<HANDLE>(audio_thread_.native_handle()),
                      THREAD_PRIORITY_ABOVE_NORMAL);
  }

  return true;
}

void ScrcpyRuntime::stop_session()
{
  close_socket(video_socket_);
  close_socket(audio_socket_);
  close_socket(listen_socket_);
  if (video_thread_.joinable()) {
    video_thread_.join();
  }
  if (audio_thread_.joinable()) {
    audio_thread_.join();
  }
  cleanup();
}

bool ScrcpyRuntime::wait_before_retry(std::chrono::milliseconds delay)
{
  constexpr auto slice = std::chrono::milliseconds(100);
  auto remaining = delay;
  while (!stopping_ && remaining.count() > 0) {
    const auto current = std::min(slice, remaining);
    std::this_thread::sleep_for(current);
    remaining -= current;
  }
  return !stopping_;
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
    std::vector<std::string> selector = build_adb_selector_args(settings_);
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
  std::vector<std::string> selector = build_adb_selector_args(settings_);
  std::vector<std::string> args = {adb_path(settings_)};
  args.insert(args.end(), selector.begin(), selector.end());
  args.push_back("shell");
  args.push_back("CLASSPATH=/data/local/tmp/scrcpy-server.jar");
  args.push_back("app_process");
  args.push_back("/");
  args.push_back("com.genymobile.scrcpy.Server");
  args.push_back(server_version_);

  char scid_text[16];
  snprintf(scid_text, sizeof(scid_text), "scid=%08x", scid_);
  args.push_back(scid_text);
  args.push_back("log_level=info");
  args.push_back("control=false");
  args.push_back("cleanup=true");
  args.push_back("send_dummy_byte=false");
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
  append_if_value(args, "video_codec_options",
                  merge_realtime_video_codec_options(settings_.video_codec_options));
  append_if_value(args, "audio_codec_options",
                  normalize_audio_codec_options(settings_.audio_codec_options));
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
  const auto wait_for_stream = [this, listen_socket](int timeout_ms) {
    constexpr int poll_ms = 100;
    int waited_ms = 0;
    while (!stopping_ && waited_ms < timeout_ms) {
      const int remaining_ms = timeout_ms - waited_ms;
      const int current_ms = std::min(poll_ms, remaining_ms);
      if (wait_for_connection(listen_socket, current_ms)) {
        return true;
      }
      waited_ms += current_ms;
    }
    return false;
  };

  if (!wait_for_stream(10000)) {
    if (stopping_) {
      return false;
    }
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

  if (!wait_for_stream(3000)) {
    if (stopping_) {
      return false;
    }
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
  struct MarkVideoEnded {
    std::atomic<bool> &flag;
    ~MarkVideoEnded()
    {
      flag = true;
    }
  } mark_video_ended{video_stream_ended_};

  SOCKET socket = as_socket(video_socket_);

  uint8_t codec_buf[4] = {};
  if (!recv_all(socket, codec_buf, 4)) {
    blog(LOG_WARNING,
         "[obs-scrcpy-source] video socket closed before codec metadata");
    return;
  }

  uint32_t raw_codec_id = read_be32(codec_buf);
  const AVCodec *codec = find_video_decoder(raw_codec_id);
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
  configure_software_decoder(ctx, session_width, session_height);
  AVPixelFormat hardware_format = AV_PIX_FMT_NONE;
  bool hardware_decode = configure_d3d11va_decoder(ctx, codec, hardware_format);

  int ret = avcodec_open2(ctx, codec, nullptr);
  if (ret < 0 && hardware_decode) {
    blog(LOG_WARNING,
         "[obs-scrcpy-source] D3D11VA decoder open failed, using software decode: %s",
         av_error(ret).c_str());
    avcodec_free_context(&ctx);
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
      return;
    }
    configure_software_decoder(ctx, session_width, session_height);
    hardware_decode = false;
    hardware_format = AV_PIX_FMT_NONE;
    ret = avcodec_open2(ctx, codec, nullptr);
  }
  if (ret < 0) {
    blog(LOG_ERROR, "[obs-scrcpy-source] could not open video decoder: %s",
         av_error(ret).c_str());
    avcodec_free_context(&ctx);
    return;
  }

  blog(LOG_INFO,
       "[obs-scrcpy-source] video diagnostics: decoder=%s path=%s threads=%d OBS buffering=%s CV bridge=%s",
       codec->name ? codec->name : "unknown",
       hardware_decode ? "D3D11VA" : "software", ctx->thread_count,
       obs_source_async_unbuffered(source_) ? "disabled" : "enabled",
       settings_.cv_bridge_enabled ? "enabled" : "disabled");

  AVFrame *frame = av_frame_alloc();
  AVFrame *software_frame = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  std::vector<uint8_t> config;
  bool config_pending = false;
  bool logged_first_frame = false;
  bool logged_unsupported_format = false;
  bool have_pts_baseline = false;
  uint64_t first_pts = 0;
  uint64_t latest_pts = 0;
  auto first_pts_received_at = std::chrono::steady_clock::now();
  auto next_diagnostics_at =
      first_pts_received_at + std::chrono::seconds(2);
  uint64_t interval_packets = 0;
  uint64_t interval_frames = 0;
  uint64_t interval_suppressed_frames = 0;
  double interval_decode_output_ms = 0.0;
  double max_decode_output_ms = 0.0;
  u_long max_socket_backlog = 0;
  LiveEdgeVideoPolicy live_edge_policy;
  const double frame_budget_ms =
      settings_.max_fps > 0 ? 1000.0 / settings_.max_fps : 1000.0 / 60.0;
  double last_decode_output_ms = 0.0;
  const auto session_started_at = std::chrono::steady_clock::now();
  const std::string video_codec_options =
      merge_realtime_video_codec_options(settings_.video_codec_options);
  blog(LOG_INFO,
       "[obs-scrcpy-source] encoder profile: codec=%s encoder=%s max_fps=%d "
       "video_codec_options=%s refresh_min=%d",
       settings_.video_codec.c_str(),
       settings_.video_encoder.empty() ? "(device default)"
                                       : settings_.video_encoder.c_str(),
       settings_.max_fps, video_codec_options.empty() ? "(none)"
                                                      : video_codec_options.c_str(),
       settings_.encoder_refresh_minutes);

  auto emit_decoded_frame = [&](const AVFrame *output_frame,
                                uint64_t timestamp_ns) -> bool {
    if (output_obs_video_frame(source_, output_frame, timestamp_ns)) {
      if (first_video_frame_at_ns_.load() == 0) {
        first_video_frame_at_ns_.store(timestamp_ns);
      }
      ++interval_frames;
      queue_bridge_frame(output_frame);
      width_ = static_cast<uint32_t>(output_frame->width);
      height_ = static_cast<uint32_t>(output_frame->height);
      if (!logged_first_frame) {
        const char *format_name = av_get_pix_fmt_name(
            static_cast<AVPixelFormat>(output_frame->format));
        blog(LOG_INFO, "[obs-scrcpy-source] output video frame %ux%u %s",
             static_cast<uint32_t>(output_frame->width),
             static_cast<uint32_t>(output_frame->height),
             format_name ? format_name : "unknown");
        logged_first_frame = true;
      }
      return true;
    } else if (!logged_unsupported_format) {
      const char *format_name =
          av_get_pix_fmt_name(static_cast<AVPixelFormat>(output_frame->format));
      blog(LOG_WARNING,
           "[obs-scrcpy-source] unsupported decoded video format %s",
           format_name ? format_name : "unknown");
      logged_unsupported_format = true;
    }
    return false;
  };

  auto transfer_and_emit_frame = [&](bool present_frame,
                                     uint64_t timestamp_ns) -> bool {
    if (!present_frame) {
      ++interval_suppressed_frames;
      av_frame_unref(frame);
      av_frame_unref(software_frame);
      return false;
    }

    const AVFrame *output_frame = frame;
    if (hardware_decode && frame->format == hardware_format) {
      av_frame_unref(software_frame);
      const int transfer_ret =
          av_hwframe_transfer_data(software_frame, frame, 0);
      if (transfer_ret < 0) {
        blog(LOG_WARNING,
             "[obs-scrcpy-source] could not transfer D3D11VA frame: %s",
             av_error(transfer_ret).c_str());
        av_frame_unref(frame);
        return false;
      }
      av_frame_copy_props(software_frame, frame);
      output_frame = software_frame;
    }

    const bool emitted = emit_decoded_frame(output_frame, timestamp_ns);
    av_frame_unref(frame);
    av_frame_unref(software_frame);
    return emitted;
  };

  auto decode_video_packet = [&](bool present_frame, bool track_presented_pts) {
    auto receive_frame = [&]() -> int {
      const int receive_ret = avcodec_receive_frame(ctx, frame);
      if (receive_ret < 0) {
        return receive_ret;
      }
      int64_t decoded_pts = frame->best_effort_timestamp;
      if (decoded_pts == AV_NOPTS_VALUE) {
        decoded_pts = frame->pts;
      }
      if (decoded_pts == AV_NOPTS_VALUE) {
        decoded_pts = packet->pts;
      }
      const uint64_t timestamp_ns =
          decoded_pts >= 0
              ? scrcpy_media_timestamp_ns(static_cast<uint64_t>(decoded_pts), 0)
              : os_gettime_ns();
      const bool emitted =
          transfer_and_emit_frame(present_frame, timestamp_ns);
      if (emitted && track_presented_pts && decoded_pts >= 0) {
        latest_presented_video_pts_us_.store(
            static_cast<uint64_t>(decoded_pts));
      }
      return 0;
    };

    int send_ret = avcodec_send_packet(ctx, packet);
    while (send_ret == AVERROR(EAGAIN)) {
      const int receive_ret = receive_frame();
      if (receive_ret == AVERROR(EAGAIN)) {
        blog(LOG_WARNING,
             "[obs-scrcpy-source] decoder stalled with a full input queue; flushing");
        avcodec_flush_buffers(ctx);
        config_pending = !config.empty();
        return;
      }
      if (receive_ret == AVERROR_EOF) {
        return;
      }
      if (receive_ret < 0) {
        blog(LOG_ERROR, "[obs-scrcpy-source] could not receive video frame: %s",
             av_error(receive_ret).c_str());
        return;
      }
      send_ret = avcodec_send_packet(ctx, packet);
    }

    if (send_ret < 0) {
      blog(LOG_WARNING,
           "[obs-scrcpy-source] could not send video packet to %s decoder: %s",
           codec->name ? codec->name : "video", av_error(send_ret).c_str());
      return;
    }

    for (;;) {
      const int receive_ret = receive_frame();
      if (receive_ret == AVERROR(EAGAIN) || receive_ret == AVERROR_EOF) {
        break;
      }
      if (receive_ret < 0) {
        blog(LOG_ERROR, "[obs-scrcpy-source] could not receive video frame: %s",
             av_error(receive_ret).c_str());
        break;
      }
    }
  };

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
    if (!packet_size) {
      break;
    }

    bool is_config = (pts_flags & kPacketFlagConfig) != 0;
    bool is_keyframe = (pts_flags & kPacketFlagKeyFrame) != 0;
    const bool merge_config_packets =
        raw_codec_id == kCodecH264 || raw_codec_id == kCodecH265;
    uint64_t packet_pts_us = pts_flags & kPacketPtsMask;
    LiveEdgeVideoAction live_edge_action =
        LiveEdgeVideoAction::DecodeAndPresent;
    bool rebase_live_edge_clock = false;
    double current_pts_drift_ms = 0.0;

    if (!is_config) {
      const auto decision_at = std::chrono::steady_clock::now();
      if (have_pts_baseline && packet_pts_us >= first_pts) {
        const double host_elapsed_ms =
            std::chrono::duration<double, std::milli>(decision_at -
                                                      first_pts_received_at)
                .count();
        const double pts_elapsed_ms =
            static_cast<double>(packet_pts_us - first_pts) / 1000.0;
        current_pts_drift_ms = host_elapsed_ms - pts_elapsed_ms;
      }

      u_long socket_backlog = 0;
      if (ioctlsocket(socket, FIONREAD, &socket_backlog) == 0) {
        max_socket_backlog = std::max(max_socket_backlog, socket_backlog);
      }
      live_edge_action =
          live_edge_policy.decide(socket_backlog, current_pts_drift_ms);
      rebase_live_edge_clock = live_edge_policy.take_clock_rebase_request();
      if (live_edge_action == LiveEdgeVideoAction::RestartStream) {
        blog(LOG_WARNING,
             "[obs-scrcpy-source] video clock discontinuity %+.1f ms; reconnecting at live edge",
             current_pts_drift_ms);
        break;
      }
    }

    if (av_new_packet(packet, packet_size) < 0) {
      break;
    }

    if (!recv_all(socket, packet->data, packet_size)) {
      av_packet_unref(packet);
      break;
    }

    if (is_config && merge_config_packets) {
      config.assign(packet->data, packet->data + packet->size);
      config_pending = true;
      av_packet_unref(packet);
      continue;
    }

    packet->pts = is_config ? AV_NOPTS_VALUE
                            : static_cast<int64_t>(pts_flags & kPacketPtsMask);
    packet->dts = packet->pts;
    if (is_keyframe) {
      packet->flags |= AV_PKT_FLAG_KEY;
    }

    if (is_config) {
      decode_video_packet(true, false);
    } else {
      const auto packet_received_at = std::chrono::steady_clock::now();
      latest_pts = packet_pts_us;
      latest_video_pts_us_.store(packet_pts_us);
      if (!have_pts_baseline || rebase_live_edge_clock) {
        have_pts_baseline = true;
        first_pts = latest_pts;
        first_pts_received_at = packet_received_at;
        next_diagnostics_at = packet_received_at + std::chrono::seconds(2);
      }

      ++interval_packets;

      if (!config.empty() && (config_pending || is_keyframe) &&
          merge_config_packets) {
        int media_size = packet->size;
        if (av_grow_packet(packet, static_cast<int>(config.size())) < 0) {
          blog(LOG_ERROR,
               "[obs-scrcpy-source] could not merge video codec config packet");
          av_packet_unref(packet);
          break;
        }
        memmove(packet->data + config.size(), packet->data, media_size);
        memcpy(packet->data, config.data(), config.size());
        config_pending = false;
      }

      const bool present_frame =
          live_edge_action == LiveEdgeVideoAction::DecodeAndPresent;
      decode_video_packet(present_frame, true);

      const auto decode_output_finished_at = std::chrono::steady_clock::now();
      const double decode_output_ms =
          std::chrono::duration<double, std::milli>(
              decode_output_finished_at - packet_received_at)
              .count();
      interval_decode_output_ms += decode_output_ms;
      max_decode_output_ms = std::max(max_decode_output_ms, decode_output_ms);
      last_decode_output_ms = decode_output_ms;
      if (decode_output_finished_at >= next_diagnostics_at) {
        const double host_elapsed_ms =
            std::chrono::duration<double, std::milli>(
                decode_output_finished_at - first_pts_received_at)
                .count();
        const double pts_elapsed_ms =
            latest_pts >= first_pts
                ? static_cast<double>(latest_pts - first_pts) / 1000.0
                : 0.0;
        const double pts_drift_ms = host_elapsed_ms - pts_elapsed_ms;
        const double average_decode_output_ms =
            interval_packets
                ? interval_decode_output_ms /
                      static_cast<double>(interval_packets)
                : 0.0;
        if (settings_.encoder_refresh_minutes > 0) {
          const auto session_age = std::chrono::duration_cast<std::chrono::minutes>(
              decode_output_finished_at - session_started_at);
          if (session_age.count() >= settings_.encoder_refresh_minutes) {
            blog(LOG_INFO,
                 "[obs-scrcpy-source] refreshing device encoder after %lld "
                 "min to clear MediaCodec buildup",
                 static_cast<long long>(session_age.count()));
            break;
          }
        }
        blog(LOG_INFO,
             "[obs-scrcpy-source] video latency: PTS drift=%+.1f ms, socket backlog<=%lu bytes, packets=%llu, frames=%llu, suppressed=%llu, decode+output avg=%.2f ms max=%.2f ms last=%.2f ms budget=%.2f ms, OBS buffering=%s",
             pts_drift_ms, static_cast<unsigned long>(max_socket_backlog),
             static_cast<unsigned long long>(interval_packets),
             static_cast<unsigned long long>(interval_frames),
             static_cast<unsigned long long>(interval_suppressed_frames),
             average_decode_output_ms, max_decode_output_ms,
             last_decode_output_ms, frame_budget_ms,
             obs_source_async_unbuffered(source_) ? "disabled" : "enabled");
        interval_packets = 0;
        interval_frames = 0;
        interval_suppressed_frames = 0;
        interval_decode_output_ms = 0.0;
        max_decode_output_ms = 0.0;
        max_socket_backlog = 0;
        next_diagnostics_at = decode_output_finished_at + std::chrono::seconds(2);
      }
    }

    av_packet_unref(packet);
  }

  obs_source_output_video(source_, nullptr);
  av_packet_free(&packet);
  av_frame_free(&software_frame);
  av_frame_free(&frame);
  avcodec_free_context(&ctx);
}

void ScrcpyRuntime::queue_bridge_frame(const AVFrame *frame)
{
  if (stopping_ || !settings_.cv_bridge_enabled) {
    return;
  }
  if (bridge_busy_.load(std::memory_order_relaxed)) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const int fps =
      std::clamp(settings_.cv_bridge_fps, 1, kCvBridgeRealtimeMaxFps);
  const auto interval = std::chrono::milliseconds(1000 / fps);
  if (last_bridge_frame_at_.time_since_epoch().count() &&
      now - last_bridge_frame_at_ < interval) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(bridge_mutex_);
    if (bridge_busy_.load(std::memory_order_relaxed) ||
        bridge_pending_frame_ != nullptr) {
      return;
    }
    AVFrame *bridge_frame = av_frame_clone(frame);
    if (!bridge_frame) {
      return;
    }
    bridge_pending_frame_ = bridge_frame;
    last_bridge_frame_at_ = now;
  }
  bridge_condition_.notify_one();
}

void ScrcpyRuntime::bridge_loop()
{
  struct BridgeBusyGuard {
    std::atomic<bool> &busy;
    ~BridgeBusyGuard()
    {
      busy.store(false, std::memory_order_relaxed);
    }
  };

  while (!stopping_) {
    AVFrame *frame = nullptr;
    {
      std::unique_lock<std::mutex> lock(bridge_mutex_);
      bridge_condition_.wait(lock, [this] {
        return stopping_ || bridge_pending_frame_ != nullptr;
      });
      if (stopping_) {
        break;
      }
      frame = bridge_pending_frame_;
      bridge_pending_frame_ = nullptr;
      bridge_busy_.store(true, std::memory_order_relaxed);
    }

    if (frame) {
      BridgeBusyGuard busy_guard{bridge_busy_};
      int width = 0;
      int height = 0;
      std::vector<uint8_t> bgra =
          make_bridge_bgra(frame,
                           std::min(settings_.cv_bridge_max_width,
                                    kCvBridgeMaxWidth),
                           width, height);
      av_frame_free(&frame);
      if (!bgra.empty()) {
        post_bridge_raw_frame(bgra, width, height);
      }
    } else {
      bridge_busy_.store(false, std::memory_order_relaxed);
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
  uint64_t skipped_audio_packets = 0;
  uint64_t emitted_audio_frames = 0;
  auto next_audio_debug_at =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);

  auto emit_audio_frame = [&]() {
    if (!swr) {
      return;
    }

    int64_t frame_pts = frame->best_effort_timestamp;
    if (frame_pts == AV_NOPTS_VALUE) {
      frame_pts = frame->pts;
    }
    if (frame_pts == AV_NOPTS_VALUE || frame_pts < 0) {
      return;
    }
    int out_samples = swr_get_out_samples(swr, frame->nb_samples);
    converted.resize(static_cast<size_t>(out_samples) * 2 * sizeof(int16_t));
    uint8_t *out_data[1] = {converted.data()};
    const int samples = swr_convert(
        swr, out_data, out_samples,
        const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
    if (samples > 0) {
      const uint64_t audio_timestamp_ns = scrcpy_media_timestamp_ns(
          static_cast<uint64_t>(frame_pts), settings_.audio_buffer_ms);
      obs_source_audio audio = {};
      audio.data[0] = converted.data();
      audio.frames = static_cast<uint32_t>(samples);
      audio.speakers = SPEAKERS_STEREO;
      audio.format = AUDIO_FORMAT_16BIT;
      audio.samples_per_sec = 48000;
      audio.timestamp = audio_timestamp_ns;
      obs_source_output_audio(source_, &audio);
      ++emitted_audio_frames;

      const auto now = std::chrono::steady_clock::now();
      if (now >= next_audio_debug_at) {
        const uint64_t presented_pts = latest_presented_video_pts_us_.load();
        const int64_t av_delta_us =
            presented_pts > 0
                ? frame_pts - static_cast<int64_t>(presented_pts)
                : 0;
        blog(LOG_INFO,
             "[obs-scrcpy-source] audio sync: A/V PTS delta=%+.1f ms, target delay=%d ms, timeline=shared-device-PTS, skipped=%llu, emitted=%llu",
             static_cast<double>(av_delta_us) / 1000.0,
             settings_.audio_buffer_ms,
             static_cast<unsigned long long>(skipped_audio_packets),
             static_cast<unsigned long long>(emitted_audio_frames));
        next_audio_debug_at = now + std::chrono::seconds(2);
      }
    }
  };

  while (!stopping_) {
    if (!recv_all(socket, header.data(), header.size())) {
      break;
    }

    uint64_t pts_flags = read_be64(header.data());
    uint32_t packet_size = read_be32(header.data() + 8);
    if (!packet_size) {
      break;
    }

    const bool is_config = (pts_flags & kPacketFlagConfig) != 0;
    const uint64_t packet_pts_us = pts_flags & kPacketPtsMask;

    if (!is_config) {
      u_long socket_backlog = 0;
      if (ioctlsocket(socket, FIONREAD, &socket_backlog) == 0) {
        (void)socket_backlog;
      }

      const uint64_t presented_pts = latest_presented_video_pts_us_.load();
      if (live_edge_audio_action(packet_pts_us, presented_pts, socket_backlog) ==
          LiveEdgeAudioAction::SkipPayload) {
        ++skipped_audio_packets;
        if (!discard_socket_data(socket, packet_size)) {
          break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_audio_debug_at) {
          const int64_t lead_us =
              presented_pts > 0
                  ? static_cast<int64_t>(packet_pts_us) -
                        static_cast<int64_t>(presented_pts)
                  : 0;
          blog(LOG_DEBUG,
               "[obs-scrcpy-source] audio live-edge recovery: lead=%lld ms, skipped=%llu, emitted=%llu",
               static_cast<long long>(lead_us / 1000),
               static_cast<unsigned long long>(skipped_audio_packets),
               static_cast<unsigned long long>(emitted_audio_frames));
          next_audio_debug_at = now + std::chrono::seconds(2);
        }
        continue;
      }
    }

    if (av_new_packet(packet, packet_size) < 0) {
      break;
    }

    if (!recv_all(socket, packet->data, packet_size)) {
      av_packet_unref(packet);
      break;
    }

    packet->pts = is_config ? AV_NOPTS_VALUE
                            : static_cast<int64_t>(packet_pts_us);
    packet->dts = packet->pts;

    auto receive_audio_frame = [&]() -> int {
      const int receive_ret = avcodec_receive_frame(ctx, frame);
      if (receive_ret >= 0) {
        emit_audio_frame();
        av_frame_unref(frame);
      }
      return receive_ret;
    };

    int send_ret = avcodec_send_packet(ctx, packet);
    while (send_ret == AVERROR(EAGAIN)) {
      const int receive_ret = receive_audio_frame();
      if (receive_ret == AVERROR(EAGAIN)) {
        blog(LOG_WARNING,
             "[obs-scrcpy-source] audio decoder stalled with a full input queue; flushing");
        avcodec_flush_buffers(ctx);
        break;
      }
      if (receive_ret == AVERROR_EOF) {
        break;
      }
      if (receive_ret < 0) {
        blog(LOG_WARNING,
             "[obs-scrcpy-source] could not receive audio frame: %s",
             av_error(receive_ret).c_str());
        break;
      }
      send_ret = avcodec_send_packet(ctx, packet);
    }

    if (send_ret >= 0) {
      for (;;) {
        const int receive_ret = receive_audio_frame();
        if (receive_ret == AVERROR(EAGAIN) || receive_ret == AVERROR_EOF) {
          break;
        }
        if (receive_ret < 0) {
          blog(LOG_WARNING,
               "[obs-scrcpy-source] could not receive audio frame: %s",
               av_error(receive_ret).c_str());
          break;
        }
      }
    } else if (send_ret != AVERROR(EAGAIN)) {
      blog(LOG_WARNING,
           "[obs-scrcpy-source] could not send audio packet to %s decoder: %s",
           codec->name ? codec->name : "audio", av_error(send_ret).c_str());
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

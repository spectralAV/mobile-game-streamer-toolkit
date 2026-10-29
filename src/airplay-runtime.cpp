#include "airplay-runtime.h"

#include "airplay-h264.h"
#include "airplay-log-filter.h"
#include "airplay-video-format.h"
#include "scrcpy-disconnected-frame.h"

#include <media-io/video-io.h>
#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

constexpr const char *kAirPlayDllName = "airplay2dll.dll";

std::string trim_string(std::string value)
{
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\r' || value.back() == '\n')) {
    value.pop_back();
  }
  size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t')) {
    ++first;
  }
  return first ? value.substr(first) : value;
}

std::string normalize_receiver_name(const std::string &name)
{
  std::string normalized = trim_string(name);
  if (normalized.empty()) {
    normalized = kDefaultAirPlayReceiverName;
  }
  if (normalized.size() >= kAirPlayNameLength) {
    normalized.resize(kAirPlayNameLength - 1);
  }
  return normalized;
}

std::string module_directory()
{
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&module_directory), &module)) {
    return {};
  }

  std::array<char, MAX_PATH> path = {};
  DWORD len = GetModuleFileNameA(module, path.data(),
                                static_cast<DWORD>(path.size()));
  if (!len || len >= path.size()) {
    return {};
  }

  std::filesystem::path module_path(path.data());
  return module_path.parent_path().string();
}

std::vector<std::string> airplay_dll_candidates(const std::string &configured)
{
  std::vector<std::string> candidates;
  if (!configured.empty()) {
    candidates.push_back(configured);
  }

  const std::string dir = module_directory();
  if (!dir.empty()) {
    candidates.push_back(
        (std::filesystem::path(dir) / kAirPlayDllName).string());
  }

  candidates.push_back(kAirPlayDllName);
  return candidates;
}

template <typename Fn>
Fn resolve_export(HMODULE library, std::initializer_list<const char *> names)
{
  for (const char *name : names) {
    FARPROC proc = GetProcAddress(library, name);
    if (proc) {
      return reinterpret_cast<Fn>(proc);
    }
  }
  return nullptr;
}

enum video_colorspace airplay_colorspace_for_height(uint32_t height)
{
  return height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
}

enum speaker_layout speaker_layout_for_channels(uint16_t channels)
{
  switch (channels) {
  case 1:
    return SPEAKERS_MONO;
  case 2:
    return SPEAKERS_STEREO;
  default:
    return SPEAKERS_UNKNOWN;
  }
}

int obs_log_level_from_airplay(int level)
{
  if (level <= 2) {
    return LOG_ERROR;
  }
  if (level <= 4) {
    return LOG_WARNING;
  }
  return LOG_INFO;
}

std::string av_error_string(int error)
{
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer = {};
  av_strerror(error, buffer.data(), buffer.size());
  return buffer.data();
}

bool is_encoded_h264_frame(const SFgVideoFrame *frame)
{
  return frame && frame->data && frame->dataLen[0] > 0 &&
         frame->dataTotalLen == frame->dataLen[0] &&
         frame->width == 0 && frame->height == 0;
}

enum video_colorspace obs_colorspace_for_avframe(const AVFrame *frame)
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

enum video_range_type obs_range_for_avframe(const AVFrame *frame)
{
  const AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
  return frame->color_range == AVCOL_RANGE_JPEG ||
                 airplay_pixel_format_implies_full_range(format)
             ? VIDEO_RANGE_FULL
             : VIDEO_RANGE_PARTIAL;
}

bool obs_format_for_avframe(const AVFrame *frame, enum video_format &format)
{
  switch (airplay_video_format_from_av_pixel_format(
      static_cast<AVPixelFormat>(frame->format))) {
  case AirPlayVideoFormat::I420:
    format = VIDEO_FORMAT_I420;
    return true;
  case AirPlayVideoFormat::NV12:
    format = VIDEO_FORMAT_NV12;
    return true;
  default:
    return false;
  }
}

bool has_readable_plane(const AVFrame *frame, int plane, size_t row_bytes,
                        int rows)
{
  if (!frame || plane < 0 || plane >= AV_NUM_DATA_POINTERS || rows <= 0 ||
      row_bytes == 0 || !frame->data[plane] || frame->linesize[plane] <= 0 ||
      static_cast<size_t>(frame->linesize[plane]) < row_bytes) {
    return false;
  }
  return true;
}

bool has_readable_8bit_layout(const AVFrame *frame, enum video_format format,
                              int width, int height)
{
  if (!frame || width <= 0 || height <= 0) {
    return false;
  }
  const int chroma_width = (width + 1) / 2;
  const int chroma_height = (height + 1) / 2;

  if (format == VIDEO_FORMAT_I420) {
    return (frame->format == AV_PIX_FMT_YUV420P ||
            frame->format == AV_PIX_FMT_YUVJ420P) &&
           has_readable_plane(frame, 0, static_cast<size_t>(width), height) &&
           has_readable_plane(frame, 1, static_cast<size_t>(chroma_width),
                              chroma_height) &&
           has_readable_plane(frame, 2, static_cast<size_t>(chroma_width),
                              chroma_height);
  }

  if (format == VIDEO_FORMAT_NV12) {
    const size_t chroma_row_bytes = static_cast<size_t>(chroma_width) * 2;
    return frame->format == AV_PIX_FMT_NV12 &&
           has_readable_plane(frame, 0, static_cast<size_t>(width), height) &&
           has_readable_plane(frame, 1, chroma_row_bytes, chroma_height);
  }

  return false;
}

bool copy_frame_to_contiguous_obs_buffer(const AVFrame *frame, int width,
                                         int height, enum video_format format,
                                         obs_source_frame &obs_frame,
                                         std::vector<uint8_t> &storage)
{
  if (!frame || !has_readable_8bit_layout(frame, format, width, height)) {
    return false;
  }

  const int chroma_width = (width + 1) / 2;
  const int chroma_height = (height + 1) / 2;
  const size_t y_size =
      static_cast<size_t>(width) * static_cast<size_t>(height);

  if (format == VIDEO_FORMAT_I420) {
    const size_t uv_size =
        static_cast<size_t>(chroma_width) * static_cast<size_t>(chroma_height);
    storage.resize(y_size + uv_size * 2);
    uint8_t *const y = storage.data();
    uint8_t *const u = y + y_size;
    uint8_t *const v = u + uv_size;

    for (int row = 0; row < height; ++row) {
      memcpy(y + static_cast<size_t>(row) * width,
             frame->data[0] + static_cast<size_t>(row) * frame->linesize[0],
             width);
    }
    for (int row = 0; row < chroma_height; ++row) {
      memcpy(u + static_cast<size_t>(row) * chroma_width,
             frame->data[1] + static_cast<size_t>(row) * frame->linesize[1],
             chroma_width);
      memcpy(v + static_cast<size_t>(row) * chroma_width,
             frame->data[2] + static_cast<size_t>(row) * frame->linesize[2],
             chroma_width);
    }

    obs_frame.data[0] = y;
    obs_frame.data[1] = u;
    obs_frame.data[2] = v;
    obs_frame.linesize[0] = static_cast<uint32_t>(width);
    obs_frame.linesize[1] = static_cast<uint32_t>(chroma_width);
    obs_frame.linesize[2] = static_cast<uint32_t>(chroma_width);
    return true;
  }

  if (format == VIDEO_FORMAT_NV12) {
    const size_t uv_stride = static_cast<size_t>(chroma_width) * 2;
    const size_t uv_size = uv_stride * static_cast<size_t>(chroma_height);
    storage.resize(y_size + uv_size);
    uint8_t *const y = storage.data();
    uint8_t *const uv = y + y_size;

    for (int row = 0; row < height; ++row) {
      memcpy(y + static_cast<size_t>(row) * width,
             frame->data[0] + static_cast<size_t>(row) * frame->linesize[0],
             width);
    }
    for (int row = 0; row < chroma_height; ++row) {
      memcpy(uv + static_cast<size_t>(row) * uv_stride,
             frame->data[1] + static_cast<size_t>(row) * frame->linesize[1],
             uv_stride);
    }

    obs_frame.data[0] = y;
    obs_frame.data[1] = uv;
    obs_frame.linesize[0] = static_cast<uint32_t>(width);
    obs_frame.linesize[1] = static_cast<uint32_t>(uv_stride);
    return true;
  }

  return false;
}

} // namespace

class AirPlayCallbackProxy : public IAirServerCallback {
public:
  explicit AirPlayCallbackProxy(AirPlayRuntime *runtime) : runtime_(runtime) {}

  void detach() { runtime_.store(nullptr); }

  void connected(const char *remoteName, const char *remoteDeviceId) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->connected(remoteName, remoteDeviceId);
    }
  }

  void disconnected(const char *remoteName,
                    const char *remoteDeviceId) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->disconnected(remoteName, remoteDeviceId);
    }
  }

  void outputAudio(SFgAudioFrame *data, const char *remoteName,
                   const char *remoteDeviceId) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->outputAudio(data, remoteName, remoteDeviceId);
    }
  }

  void outputH264AccessUnit(SFgH264AccessUnit *data, const char *remoteName,
                            const char *remoteDeviceId) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->outputH264AccessUnit(data, remoteName, remoteDeviceId);
    }
  }

  void outputVideo(SFgVideoFrame *data, const char *remoteName,
                   const char *remoteDeviceId) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->outputVideo(data, remoteName, remoteDeviceId);
    }
  }

  void videoGeometryChanged(float sourceWidth, float sourceHeight,
                            float outputWidth, float outputHeight,
                            const char *remoteName,
                            const char *remoteDeviceId) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->videoGeometryChanged(sourceWidth, sourceHeight, outputWidth,
                               outputHeight, remoteName, remoteDeviceId);
    }
  }

  void videoSenderPausedChanged(bool paused, const char *remoteName,
                                const char *remoteDeviceId) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->videoSenderPausedChanged(paused, remoteName, remoteDeviceId);
    }
  }

  void videoPlay(char *url, double volume, double startPos) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->videoPlay(url, volume, startPos);
    }
  }

  void videoGetPlayInfo(double *duration, double *position,
                        double *rate) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->videoGetPlayInfo(duration, position, rate);
    } else {
      if (duration) *duration = 0.0;
      if (position) *position = 0.0;
      if (rate) *rate = 0.0;
    }
  }

  void setVolume(float volume, const char *remoteName,
                 const char *remoteDeviceId) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->setVolume(volume, remoteName, remoteDeviceId);
    }
  }

  bool approvePairingRequest(const char *remoteName,
                             const char *remoteDeviceId,
                             const char *remoteModel,
                             const char *remoteOsName,
                             const char *remoteOsVersion,
                             const char *remoteOsBuildVersion,
                             const char *remoteSourceVersion,
                             const char *pairingFingerprint) override
  {
    AirPlayRuntime *rt = runtime_.load();
    return rt ? rt->approvePairingRequest(
                    remoteName, remoteDeviceId, remoteModel, remoteOsName,
                    remoteOsVersion, remoteOsBuildVersion,
                    remoteSourceVersion, pairingFingerprint)
              : false;
  }

  bool requestPinApproval(const char *remoteAddress,
                          const char *pin) override
  {
    AirPlayRuntime *rt = runtime_.load();
    return rt ? rt->requestPinApproval(remoteAddress, pin) : false;
  }

  void log(int level, const char *msg) override
  {
    AirPlayRuntime *rt = runtime_.load();
    if (rt) {
      rt->log(level, msg);
    }
  }

private:
  std::atomic<AirPlayRuntime *> runtime_{nullptr};
};

AirPlayRuntime::AirPlayRuntime(obs_source_t *source) : source_(source) {}

AirPlayRuntime::~AirPlayRuntime()
{
  stop();
  unload_library();
}

bool AirPlayRuntime::start(const AirPlaySettings &settings)
{
  stop();

  if (last_teardown_completed_ && !last_teardown_completed_->load()) {
    for (int i = 0; i < 50 && !last_teardown_completed_->load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  settings_ = settings;
  settings_.receiver_name = normalize_receiver_name(settings_.receiver_name);
  stopping_ = false;
  unsupported_media_logged_ = false;
  unsupported_audio_logged_ = false;
  disconnected_frame_visible_ = false;

  if (!logged_first_video_frame_ && settings_.display_width > 0 &&
      settings_.display_height > 0) {
    width_ = settings_.display_width;
    height_ = settings_.display_height;
  }

  if (settings_.show_disconnected_frame) {
    output_disconnected_frame();
  } else {
    obs_source_output_video(source_, nullptr);
  }

  if (!load_library()) {
    stopping_ = true;
    disconnected_frame_visible_ = false;
    if (source_) {
      obs_source_output_video(source_, nullptr);
    }
    return false;
  }

  callback_proxy_ = std::make_shared<AirPlayCallbackProxy>(this);

  std::array<char, kAirPlayNameLength> receiver_name = {};
  strncpy_s(receiver_name.data(), receiver_name.size(),
            settings_.receiver_name.c_str(), _TRUNCATE);

  if (fg_server_start_with_display_ && settings_.display_width > 0 &&
      settings_.display_height > 0) {
    server_ = fg_server_start_with_display_(
        receiver_name.data(), static_cast<unsigned int>(settings_.raop_port),
        static_cast<unsigned int>(settings_.airplay_port),
        callback_proxy_.get(), nullptr, settings_.display_width,
        settings_.display_height);
    blog(LOG_INFO,
         "[obs-airplay-source] receiver advertised canvas: %ux%u (%s)",
         settings_.display_width, settings_.display_height,
         settings_.resolution.c_str());
  } else if (fg_server_start_headless_) {
    server_ = fg_server_start_headless_(
        receiver_name.data(), static_cast<unsigned int>(settings_.raop_port),
        static_cast<unsigned int>(settings_.airplay_port),
        callback_proxy_.get());
  } else if (fg_server_start_) {
    server_ = fg_server_start_(
        receiver_name.data(), static_cast<unsigned int>(settings_.raop_port),
        static_cast<unsigned int>(settings_.airplay_port),
        callback_proxy_.get(), nullptr);
  } else if (fg_server_start_legacy_) {
    server_ = fg_server_start_legacy_(
        receiver_name.data(), static_cast<unsigned int>(settings_.raop_port),
        static_cast<unsigned int>(settings_.airplay_port),
        callback_proxy_.get());
  }

  if (!server_) {
    blog(LOG_ERROR,
         "[obs-airplay-source] AirPlay receiver failed to start on ports %d/%d",
         settings_.raop_port, settings_.airplay_port);
    if (callback_proxy_) {
      callback_proxy_->detach();
      callback_proxy_.reset();
    }
    stopping_ = true;
    disconnected_frame_visible_ = false;
    if (source_) {
      obs_source_output_video(source_, nullptr);
    }
    return false;
  }

  running_ = true;
  blog(LOG_INFO,
       "[obs-airplay-source] receiver '%s' started on RAOP port %d and AirPlay port %d",
       settings_.receiver_name.c_str(), settings_.raop_port,
       settings_.airplay_port);
  return true;
}

void AirPlayRuntime::stop()
{
  stopping_ = true;
  running_ = false;

  void *server = nullptr;
  FgServerStopFn stop_fn = nullptr;
  std::shared_ptr<AirPlayCallbackProxy> proxy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    server = server_;
    server_ = nullptr;
    stop_fn = fg_server_stop_;
    proxy = std::move(callback_proxy_);
    active_device_id_.clear();
    active_device_name_.clear();
  }

  if (proxy) {
    proxy->detach();
  }

  clear_video();
  {
    std::lock_guard<std::mutex> audio_lock(audio_mutex_);
    next_audio_ts_ = 0;
    audio_frames_emitted_ = 0;
    next_audio_log_ns_ = 0;
  }
  {
    std::lock_guard<std::mutex> video_lock(video_mutex_);
    reset_h264_decoder();
    config_pending_ = true;
  }

  if (server && stop_fn) {
    auto stop_completed = std::make_shared<std::atomic<bool>>(false);
    last_teardown_completed_ = stop_completed;

    std::thread teardown_thread([server, stop_fn, proxy, stop_completed]() {
      blog(LOG_INFO, "[obs-airplay-source] stopping server instance...");
      stop_fn(server);
      stop_completed->store(true);
      blog(LOG_INFO, "[obs-airplay-source] server instance stopped cleanly");
    });

    constexpr int kStopTimeoutMs = 300;
    constexpr int kPollIntervalMs = 10;
    int elapsed = 0;
    while (!stop_completed->load() && elapsed < kStopTimeoutMs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
      elapsed += kPollIntervalMs;
    }

    if (stop_completed->load()) {
      if (teardown_thread.joinable()) {
        teardown_thread.join();
      }
    } else {
      blog(LOG_WARNING,
           "[obs-airplay-source] airplay server teardown took longer than %dms; "
           "continuing in background to prevent freezing OBS shutdown",
           kStopTimeoutMs);
      teardown_thread.detach();
    }
  }
}

uint32_t AirPlayRuntime::width() const
{
  return width_.load();
}

uint32_t AirPlayRuntime::height() const
{
  return height_.load();
}

bool AirPlayRuntime::load_library()
{
  if (library_) {
    return true;
  }

  for (const std::string &candidate :
       airplay_dll_candidates(settings_.dll_path)) {
    if (candidate.empty()) {
      continue;
    }

    const bool has_directory =
        candidate.find_first_of("/\\") != std::string::npos;
    HMODULE library =
        has_directory
            ? LoadLibraryExA(candidate.c_str(), nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH)
            : LoadLibraryA(candidate.c_str());
    if (!library) {
      continue;
    }

    fg_server_start_headless_ = resolve_export<FgServerStartHeadlessFn>(
        library,
        {"?fgServerStartHeadless@@YAPEAXQEBDIIPEAVIAirServerCallback@@@Z",
         "fgServerStartHeadless"});
    fg_server_start_with_display_ = resolve_export<FgServerStartWithDisplayFn>(
        library,
        {"?fgServerStartWithDisplay@@YAPEAXQEBDIIPEAVIAirServerCallback@@PEBDII@Z",
         "fgServerStartWithDisplay"});
    fg_server_start_ = resolve_export<FgServerStartFn>(
        library,
        {"?fgServerStart@@YAPEAXQEBDIIPEAVIAirServerCallback@@PEBD@Z",
         "?fgServerStart@@YAPEAXPEBDIIPEAVIAirServerCallback@@PEBD@Z"});
    fg_server_start_legacy_ = resolve_export<FgServerStartLegacyFn>(
        library,
        {"fgServerStart",
         "?fgServerStart@@YAPEAXPEBDIIPEAVIAirServerCallback@@@Z",
         "?fgServerStart@@YAPEAXQEBDIIPEAVIAirServerCallback@@@Z"});
    fg_server_stop_ = resolve_export<FgServerStopFn>(
        library, {"fgServerStop", "?fgServerStop@@YAXPEAX@Z"});
    fg_server_scale_ = resolve_export<FgServerScaleFn>(
        library, {"fgServerScale", "?fgServerScale@@YAMPEAXM@Z"});

    if ((fg_server_start_headless_ || fg_server_start_with_display_ ||
         fg_server_start_ || fg_server_start_legacy_) &&
        fg_server_stop_) {
      library_ = library;
      blog(LOG_INFO, "[obs-airplay-source] loaded %s (headless=%s)",
           candidate.c_str(), fg_server_start_headless_ ? "yes" : "no");
      return true;
    }

    FreeLibrary(library);
  }

  blog(LOG_ERROR,
       "[obs-airplay-source] could not load %s. Place it next to the OBS plugin DLL or select it in the AirPlay source settings.",
       kAirPlayDllName);
  return false;
}

void AirPlayRuntime::unload_library()
{
  // Do NOT call FreeLibrary on airplay2dll during source destruction.
  // airplay2dll manages active Winsock, crypto, Bonjour, and thread pool resources.
  // Calling FreeLibrary() unmaps code pages while background helper threads may be
  // unwinding, triggering Windows Loader Lock deadlocks and access violations that
  // crash OBS during shutdown (causing the "OBS did not shut down properly" dialog).
  // Keeping the module pinned ensures Windows unloads it safely at process termination.
  library_ = nullptr;
  fg_server_start_headless_ = nullptr;
  fg_server_start_with_display_ = nullptr;
  fg_server_start_ = nullptr;
  fg_server_start_legacy_ = nullptr;
  fg_server_stop_ = nullptr;
  fg_server_scale_ = nullptr;
}

bool AirPlayRuntime::should_accept_callback(const char *remote_device_id)
{
  if (stopping_) {
    return false;
  }
  if (!remote_device_id || !remote_device_id[0] || active_device_id_.empty()) {
    return true;
  }
  return active_device_id_ == remote_device_id;
}

void AirPlayRuntime::clear_video()
{
  if (stopping_ || !settings_.show_disconnected_frame) {
    disconnected_frame_visible_ = false;
    if (source_) {
      obs_source_output_video(source_, nullptr);
    }
    return;
  }

  output_disconnected_frame();
}

void AirPlayRuntime::output_disconnected_frame()
{
  if (stopping_ || !settings_.show_disconnected_frame) {
    return;
  }

  bool expected = false;
  if (!disconnected_frame_visible_.compare_exchange_strong(expected, true)) {
    return;
  }

  uint32_t w = width_.load();
  uint32_t h = height_.load();
  if (w == 0 || h == 0) {
    w = settings_.display_width > 0 ? settings_.display_width : 1920;
    h = settings_.display_height > 0 ? settings_.display_height : 1080;
  }

  ScrcpyDisconnectedFrame disconnected =
      make_scrcpy_disconnected_frame(w, h);
  if (disconnected.bgra.empty()) {
    disconnected_frame_visible_ = false;
    blog(LOG_WARNING,
         "[obs-airplay-source] could not create disconnected frame for %ux%u",
         w, h);
    return;
  }

  obs_source_frame frame = {};
  frame.data[0] = disconnected.bgra.data();
  frame.linesize[0] = disconnected.stride;
  frame.width = disconnected.width;
  frame.height = disconnected.height;
  frame.timestamp = os_gettime_ns();
  frame.format = VIDEO_FORMAT_BGRA;
  frame.full_range = true;
  video_format_get_parameters_for_format(
      VIDEO_CS_SRGB, VIDEO_RANGE_FULL, frame.format, frame.color_matrix,
      frame.color_range_min, frame.color_range_max);
  obs_source_output_video(source_, &frame);
  blog(LOG_INFO,
       "[obs-airplay-source] showing device disconnected frame at %ux%u",
       disconnected.width, disconnected.height);
}

void AirPlayRuntime::output_airplay_video(const SFgVideoFrame *frame)
{
  if (is_encoded_h264_frame(frame)) {
    output_encoded_h264_payload(frame->data, frame->dataLen[0], frame->pts,
                                frame->isKey);
    return;
  }

  if (!frame || !frame->data ||
      !airplay_is_valid_video_geometry(static_cast<int>(frame->width),
                                       static_cast<int>(frame->height))) {
    return;
  }

  const uint32_t w = frame->width;
  const uint32_t h = frame->height;
  const uint32_t chroma_w = (w + 1) / 2;
  const uint32_t chroma_h = h / 2;
  const uint32_t obs_chroma_h = (h + 1) / 2;

  if (frame->pitch[0] < w) {
    return;
  }
  if (chroma_h > 0 &&
      (frame->pitch[1] < chroma_w || frame->pitch[2] < chroma_w)) {
    return;
  }
  const size_t req_y = static_cast<size_t>(frame->pitch[0]) * h;
  const size_t req_u = static_cast<size_t>(frame->pitch[1]) * chroma_h;
  const size_t req_v = static_cast<size_t>(frame->pitch[2]) * chroma_h;
  if (frame->dataLen[0] < req_y || frame->dataLen[1] < req_u ||
      frame->dataLen[2] < req_v) {
    return;
  }
  if (frame->dataTotalLen > 0 &&
      (req_y + req_u + req_v > frame->dataTotalLen)) {
    return;
  }

  const uint64_t timestamp_ns = os_gettime_ns();
  obs_source_frame obs_frame = {};
  obs_frame.format = VIDEO_FORMAT_I420;
  obs_frame.width = w;
  obs_frame.height = h;
  obs_frame.timestamp = timestamp_ns;
  obs_frame.full_range = false;
  video_format_get_parameters_for_format(
      airplay_colorspace_for_height(h), VIDEO_RANGE_PARTIAL,
      obs_frame.format, obs_frame.color_matrix, obs_frame.color_range_min,
      obs_frame.color_range_max);

  const size_t y_size = static_cast<size_t>(w) * h;
  const size_t uv_size = static_cast<size_t>(chroma_w) * obs_chroma_h;
  video_buffer_storage_.resize(y_size + uv_size * 2);
  uint8_t *const y = video_buffer_storage_.data();
  uint8_t *const u = y + y_size;
  uint8_t *const v = u + uv_size;

  const uint8_t *const src_y = frame->data;
  const uint8_t *const src_u = frame->data + frame->dataLen[0];
  const uint8_t *const src_v =
      frame->data + frame->dataLen[0] + frame->dataLen[1];

  for (uint32_t row = 0; row < h; ++row) {
    memcpy(y + static_cast<size_t>(row) * w,
           src_y + static_cast<size_t>(row) * frame->pitch[0], w);
  }
  for (uint32_t row = 0; row < obs_chroma_h; ++row) {
    const uint32_t src_row =
        (row < chroma_h) ? row : (chroma_h > 0 ? chroma_h - 1 : 0);
    memcpy(u + static_cast<size_t>(row) * chroma_w,
           src_u + static_cast<size_t>(src_row) * frame->pitch[1], chroma_w);
    memcpy(v + static_cast<size_t>(row) * chroma_w,
           src_v + static_cast<size_t>(src_row) * frame->pitch[2], chroma_w);
  }

  obs_frame.data[0] = y;
  obs_frame.data[1] = u;
  obs_frame.data[2] = v;
  obs_frame.linesize[0] = w;
  obs_frame.linesize[1] = chroma_w;
  obs_frame.linesize[2] = chroma_w;

  obs_source_output_video(source_, &obs_frame);
  disconnected_frame_visible_ = false;
  width_ = w;
  height_ = h;
}

bool AirPlayRuntime::ensure_h264_decoder()
{
  if (video_decoder_) {
    return true;
  }

  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    blog(LOG_ERROR, "[obs-airplay-source] H.264 decoder not available");
    return false;
  }

  video_decoder_ = avcodec_alloc_context3(codec);
  video_frame_ = av_frame_alloc();
  video_packet_ = av_packet_alloc();
  if (!video_decoder_ || !video_frame_ || !video_packet_) {
    blog(LOG_ERROR, "[obs-airplay-source] could not allocate H.264 decoder");
    reset_h264_decoder();
    return false;
  }

  video_decoder_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  video_decoder_->thread_count = 1;
  video_decoder_->thread_type = FF_THREAD_SLICE;

  const int ret = avcodec_open2(video_decoder_, codec, nullptr);
  if (ret < 0) {
    blog(LOG_ERROR, "[obs-airplay-source] could not open H.264 decoder: %s",
         av_error_string(ret).c_str());
    reset_h264_decoder();
    return false;
  }

  blog(LOG_INFO, "[obs-airplay-source] using OBS-side H.264 decoder");
  return true;
}

void AirPlayRuntime::reset_h264_decoder()
{
  if (video_packet_) {
    av_packet_free(&video_packet_);
  }
  if (video_frame_) {
    av_frame_free(&video_frame_);
  }
  if (video_decoder_) {
    avcodec_free_context(&video_decoder_);
  }
  logged_first_video_frame_ = false;
  logged_first_audio_frame_ = false;
  logged_encoded_video_path_ = false;
  logged_unsupported_video_format_ = false;
}

void AirPlayRuntime::output_decoded_video_frame(const AVFrame *frame)
{
  if (!frame ||
      !airplay_is_valid_video_geometry(frame->width, frame->height)) {
    return;
  }

  enum video_format obs_format = VIDEO_FORMAT_NONE;
  if (!obs_format_for_avframe(frame, obs_format)) {
    if (!logged_unsupported_video_format_) {
      const char *format_name =
          av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
      blog(LOG_WARNING,
           "[obs-airplay-source] unsupported decoded video format %s",
           format_name ? format_name : "unknown");
      logged_unsupported_video_format_ = true;
    }
    return;
  }

  obs_source_frame obs_frame = {};
  obs_frame.format = obs_format;
  obs_frame.width = static_cast<uint32_t>(frame->width);
  obs_frame.height = static_cast<uint32_t>(frame->height);
  obs_frame.timestamp = os_gettime_ns();
  obs_frame.full_range = obs_range_for_avframe(frame) == VIDEO_RANGE_FULL;
  video_format_get_parameters_for_format(
      obs_colorspace_for_avframe(frame), obs_range_for_avframe(frame),
      obs_frame.format, obs_frame.color_matrix, obs_frame.color_range_min,
      obs_frame.color_range_max);

  if (!copy_frame_to_contiguous_obs_buffer(frame, frame->width, frame->height,
                                           obs_format, obs_frame,
                                           video_buffer_storage_)) {
    return;
  }

  obs_source_output_video(source_, &obs_frame);
  disconnected_frame_visible_ = false;
  width_ = static_cast<uint32_t>(frame->width);
  height_ = static_cast<uint32_t>(frame->height);

  if (!logged_first_video_frame_) {
    const char *format_name =
        av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
    blog(LOG_INFO, "[obs-airplay-source] output video frame %ux%u %s",
         static_cast<uint32_t>(frame->width),
         static_cast<uint32_t>(frame->height),
         format_name ? format_name : "unknown");
    logged_first_video_frame_ = true;
  }
}

std::string AirPlayRuntime::get_sps_pps_cache_path() const
{
  char *config_dir = obs_module_config_path("");
  if (config_dir) {
    std::string path = config_dir;
    bfree(config_dir);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path + "/airplay_sps_pps.bin";
  }
  return "";
}

void AirPlayRuntime::save_sps_pps_cache()
{
  if (cached_sps_pps_.empty()) {
    return;
  }
  const std::string path = get_sps_pps_cache_path();
  if (path.empty()) {
    return;
  }
  FILE *f = fopen(path.c_str(), "wb");
  if (f) {
    const uint32_t magic = 0x53505350; // 'SPSP'
    const uint32_t width = width_.load();
    const uint32_t height = height_.load();
    const uint32_t len = static_cast<uint32_t>(cached_sps_pps_.size());
    fwrite(&magic, 4, 1, f);
    fwrite(&width, 4, 1, f);
    fwrite(&height, 4, 1, f);
    fwrite(&len, 4, 1, f);
    fwrite(cached_sps_pps_.data(), 1, len, f);
    fclose(f);
  }
}

void AirPlayRuntime::load_sps_pps_cache()
{
  if (!cached_sps_pps_.empty()) {
    return;
  }
  const std::string path = get_sps_pps_cache_path();
  if (!path.empty()) {
    FILE *f = fopen(path.c_str(), "rb");
    if (f) {
      uint32_t magic = 0;
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t len = 0;
      if (fread(&magic, 4, 1, f) == 1 && magic == 0x53505350 &&
          fread(&width, 4, 1, f) == 1 &&
          fread(&height, 4, 1, f) == 1 &&
          fread(&len, 4, 1, f) == 1 && len > 0 && len < 4096) {
        std::vector<uint8_t> buf(len);
        if (fread(buf.data(), 1, len, f) == len) {
          cached_sps_pps_ = std::move(buf);
          config_pending_ = true;
          if (width > 0 && height > 0) {
            width_ = width;
            height_ = height;
          }
          blog(LOG_INFO,
               "[obs-airplay-source] loaded cached %u bytes SPS/PPS parameter set (%ux%u)",
               len, width, height);
        }
      }
      fclose(f);
    }
  }

  // Built-in fallback parameter set for iPhone (2560x1182 High@L5.1)
  if (cached_sps_pps_.empty()) {
    static const uint8_t kDefaultIPhoneSpsPps[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x33,
        0xac, 0xd9, 0x40, 0x28, 0x00, 0x95, 0xfa, 0xc0,
        0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00,
        0x03, 0x01, 0xe0, 0x3c, 0x60, 0xc6, 0x58,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xe3, 0xcb,
        0x22, 0xc0
    };
    cached_sps_pps_.assign(kDefaultIPhoneSpsPps,
                           kDefaultIPhoneSpsPps + sizeof(kDefaultIPhoneSpsPps));
    config_pending_ = true;
    width_ = 2560;
    height_ = 1182;
    blog(LOG_INFO,
         "[obs-airplay-source] using fallback SPS/PPS parameter set (2560x1182)");
  }
}

bool AirPlayRuntime::output_encoded_h264_payload(const uint8_t *payload,
                                                size_t payload_len,
                                                uint64_t pts, int is_key)
{
  if (!payload || payload_len == 0) {
    return false;
  }

  if (!ensure_h264_decoder()) {
    return false;
  }

  if (!logged_encoded_video_path_) {
    blog(LOG_INFO, "[obs-airplay-source] receiving encoded AirPlay H.264");
    logged_encoded_video_path_ = true;
  }

  if (cached_sps_pps_.empty()) {
    load_sps_pps_cache();
  }

  // AirPlay sends standalone SPS/PPS parameter sets before video frames.
  // Feeding parameter sets alone to avcodec_send_packet returns AVERROR_INVALIDDATA.
  // We extract and cache parameter sets so they can be prepended to keyframes.
  if (airplay_is_h264_parameter_set_only(payload, payload_len)) {
    const std::vector<uint8_t> sps_pps = airplay_extract_sps_pps(payload, payload_len);
    if (!sps_pps.empty()) {
      cached_sps_pps_ = sps_pps;
      config_pending_ = true;
      save_sps_pps_cache();
      blog(LOG_INFO, "[obs-airplay-source] cached %zu bytes SPS/PPS parameter set", cached_sps_pps_.size());
    }
    return true;
  }

  // If in-band SPS is present, extract only the parameter sets (excluding video slice data).
  if (airplay_h264_contains_sps(payload, payload_len)) {
    const std::vector<uint8_t> sps_pps = airplay_extract_sps_pps(payload, payload_len);
    if (!sps_pps.empty()) {
      cached_sps_pps_ = sps_pps;
      config_pending_ = false;
      save_sps_pps_cache();
    }
  }

  const std::vector<uint8_t> prepared = airplay_prepare_h264_packet(
      payload, payload_len, is_key, config_pending_, cached_sps_pps_);
  if (prepared.size() > payload_len) {
    config_pending_ = false;
  }

  if (av_new_packet(video_packet_, static_cast<int>(prepared.size())) < 0) {
    blog(LOG_WARNING, "[obs-airplay-source] could not allocate H.264 packet");
    return false;
  }
  memcpy(video_packet_->data, prepared.data(), prepared.size());
  video_packet_->pts = pts ? static_cast<int64_t>(pts) : AV_NOPTS_VALUE;
  video_packet_->dts = video_packet_->pts;
  if (is_key || airplay_h264_contains_idr(prepared.data(), prepared.size())) {
    video_packet_->flags |= AV_PKT_FLAG_KEY;
  }

  auto receive_frame = [&]() -> int {
    const int receive_ret =
        avcodec_receive_frame(video_decoder_, video_frame_);
    if (receive_ret >= 0) {
      output_decoded_video_frame(video_frame_);
      av_frame_unref(video_frame_);
    }
    return receive_ret;
  };

  int ret = avcodec_send_packet(video_decoder_, video_packet_);
  while (ret == AVERROR(EAGAIN)) {
    const int receive_ret = receive_frame();
    if (receive_ret < 0) {
      av_packet_unref(video_packet_);
      blog(LOG_WARNING,
           "[obs-airplay-source] decoder rejected input while no frame was available: %s",
           av_error_string(receive_ret).c_str());
      return false;
    }
    ret = avcodec_send_packet(video_decoder_, video_packet_);
  }
  av_packet_unref(video_packet_);
  if (ret < 0) {
    if (ret == AVERROR_INVALIDDATA && video_decoder_) {
      avcodec_flush_buffers(video_decoder_);
      config_pending_ = true;
    }
    static uint64_t last_log_time = 0;
    const uint64_t now = os_gettime_ns();
    if (now - last_log_time > 2000000000ULL) {
      last_log_time = now;
      blog(LOG_WARNING,
           "[obs-airplay-source] could not send H.264 packet to decoder: %s",
           av_error_string(ret).c_str());
    }
    return false;
  }

  for (;;) {
    ret = receive_frame();
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    }
    if (ret < 0) {
      blog(LOG_WARNING,
           "[obs-airplay-source] could not receive H.264 frame: %s",
           av_error_string(ret).c_str());
      return false;
    }
  }
  return true;
}

void AirPlayRuntime::output_airplay_audio(const SFgAudioFrame *frame)
{
  if (!frame || !frame->data) {
    return;
  }

  if (!airplay_is_valid_audio_frame(frame->sampleRate, frame->channels,
                                   frame->bitsPerSample, frame->dataLen)) {
    if (!unsupported_audio_logged_.exchange(true)) {
      blog(LOG_WARNING,
           "[obs-airplay-source] unsupported or invalid audio format: %u Hz, %u channels, %u bits, %u bytes",
           frame->sampleRate, frame->channels, frame->bitsPerSample,
           frame->dataLen);
    }
    return;
  }

  const uint32_t bytes_per_frame =
      static_cast<uint32_t>(frame->channels) * (frame->bitsPerSample / 8);
  const uint32_t num_frames = frame->dataLen / bytes_per_frame;

  const uint64_t now_ns = os_gettime_ns();
  const uint64_t frame_duration_ns =
      (static_cast<uint64_t>(num_frames) * 1000000000ull) / frame->sampleRate;

  uint64_t audio_ts = 0;
  bool should_log_stats = false;
  uint64_t total_frames = 0;
  {
    std::lock_guard<std::mutex> audio_lock(audio_mutex_);
    const int64_t delta_ns = (next_audio_ts_ > 0)
        ? (static_cast<int64_t>(next_audio_ts_) - static_cast<int64_t>(now_ns))
        : 0;

    // Smooth playback: advance continuously by frame duration.
    // Only resync if uninitialized, or if audio timestamp drifted behind real-time
    // by > 40ms (gap/silence in playback), or drifted ahead by > 80ms.
    if (next_audio_ts_ == 0 || delta_ns < -40000000ll || delta_ns > 80000000ll) {
      next_audio_ts_ = now_ns;
    }
    audio_ts = next_audio_ts_;
    next_audio_ts_ += frame_duration_ns;

    audio_frames_emitted_ += num_frames;
    total_frames = audio_frames_emitted_;
    if (now_ns >= next_audio_log_ns_) {
      should_log_stats = true;
      next_audio_log_ns_ = now_ns + 5000000000ull;
    }
  }

  obs_source_audio audio = {};
  audio.data[0] = frame->data;
  audio.frames = num_frames;
  audio.speakers = speaker_layout_for_channels(frame->channels);
  audio.format = AUDIO_FORMAT_16BIT;
  audio.samples_per_sec = frame->sampleRate;
  audio.timestamp = audio_ts;
  obs_source_output_audio(source_, &audio);

  if (!logged_first_audio_frame_.exchange(true)) {
    blog(LOG_INFO,
         "[obs-airplay-source] output audio frame %u Hz, %u channels, %u-bit, %u frames",
         frame->sampleRate, frame->channels, frame->bitsPerSample,
         audio.frames);
  } else if (should_log_stats) {
    blog(LOG_INFO,
         "[obs-airplay-source] audio active: %llu frames delivered (%u Hz, %u ch)",
         static_cast<unsigned long long>(total_frames), frame->sampleRate,
         frame->channels);
  }
}

void AirPlayRuntime::log_media_cast_unsupported_once(const char *url)
{
  if (unsupported_media_logged_.exchange(true)) {
    return;
  }
  blog(LOG_WARNING,
       "[obs-airplay-source] AirPlay media casting is not supported by this source: %s",
       url && url[0] ? url : "(no URL)");
}

void AirPlayRuntime::connected(const char *remoteName,
                               const char *remoteDeviceId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!should_accept_callback(remoteDeviceId)) {
    blog(LOG_WARNING,
         "[obs-airplay-source] ignoring AirPlay device '%s' while '%s' is active",
         remoteName ? remoteName : "(unknown)",
         active_device_name_.empty() ? active_device_id_.c_str()
                                     : active_device_name_.c_str());
    return;
  }

  if (remoteDeviceId && remoteDeviceId[0]) {
    active_device_id_ = remoteDeviceId;
  }
  active_device_name_ = remoteName ? remoteName : "";
  blog(LOG_INFO, "[obs-airplay-source] connected: %s",
       active_device_name_.empty() ? "(unknown)" : active_device_name_.c_str());
}

void AirPlayRuntime::disconnected(const char *remoteName,
                                  const char *remoteDeviceId)
{
  bool clear = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!remoteDeviceId || !remoteDeviceId[0] || active_device_id_.empty() ||
        active_device_id_ == remoteDeviceId) {
      active_device_id_.clear();
      active_device_name_.clear();
      clear = true;
    }
  }

  if (clear) {
    using_h264_access_units_ = false;
    blog(LOG_INFO, "[obs-airplay-source] disconnected: %s",
         remoteName && remoteName[0] ? remoteName : "(unknown)");
    {
      std::lock_guard<std::mutex> audio_lock(audio_mutex_);
      next_audio_ts_ = 0;
      audio_frames_emitted_ = 0;
      next_audio_log_ns_ = 0;
    }
    {
      std::lock_guard<std::mutex> video_lock(video_mutex_);
      reset_h264_decoder();
      config_pending_ = true;
    }
    clear_video();
  }
}

void AirPlayRuntime::outputAudio(SFgAudioFrame *data, const char *,
                                 const char *)
{
  if (!data || stopping_) {
    return;
  }

  output_airplay_audio(data);
}

void AirPlayRuntime::outputH264AccessUnit(SFgH264AccessUnit *data, const char *,
                                          const char *remoteDeviceId)
{
  if (!data || !data->data || data->dataLen == 0) {
    return;
  }

  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepted = should_accept_callback(remoteDeviceId);
    if (accepted && active_device_id_.empty() && remoteDeviceId &&
        remoteDeviceId[0]) {
      active_device_id_ = remoteDeviceId;
    }
  }

  if (accepted) {
    std::lock_guard<std::mutex> video_lock(video_mutex_);
    if (output_encoded_h264_payload(data->data, data->dataLen, data->pts,
                                    data->isKey)) {
      using_h264_access_units_ = true;
    }
  }
}

void AirPlayRuntime::outputVideo(SFgVideoFrame *data, const char *,
                                 const char *remoteDeviceId)
{
  if (!data) {
    return;
  }

  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepted = should_accept_callback(remoteDeviceId);
    if (accepted && active_device_id_.empty() && remoteDeviceId &&
        remoteDeviceId[0]) {
      active_device_id_ = remoteDeviceId;
    }
  }

  if (accepted) {
    std::lock_guard<std::mutex> video_lock(video_mutex_);
    // When outputH264AccessUnit is active and decoding frames, do not output
    // duplicate YUV frames from airplay2dll's secondary internal decoder.
    if (using_h264_access_units_ && !is_encoded_h264_frame(data)) {
      return;
    }
    output_airplay_video(data);
  }
}

void AirPlayRuntime::videoGeometryChanged(float sourceWidth, float sourceHeight,
                                          float outputWidth, float outputHeight,
                                          const char *,
                                          const char *remoteDeviceId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!should_accept_callback(remoteDeviceId)) {
    return;
  }

  const uint32_t w = static_cast<uint32_t>(
      outputWidth > 0 ? outputWidth : sourceWidth);
  const uint32_t h = static_cast<uint32_t>(
      outputHeight > 0 ? outputHeight : sourceHeight);
  if (airplay_is_valid_video_geometry(static_cast<int>(w),
                                      static_cast<int>(h))) {
    width_ = w;
    height_ = h;
    blog(LOG_INFO, "[obs-airplay-source] video geometry updated: %ux%u", w, h);
  }
}

void AirPlayRuntime::videoSenderPausedChanged(bool paused, const char *,
                                              const char *remoteDeviceId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!should_accept_callback(remoteDeviceId)) {
    return;
  }

  blog(LOG_INFO, "[obs-airplay-source] sender paused: %s",
       paused ? "true" : "false");
  // Do NOT clear video here. Pauses happen momentarily during app switching and
  // horizontal/vertical orientation changes; preserving the last frame prevents
  // black flicker and sprite errors.
}

void AirPlayRuntime::videoPlay(char *url, double, double)
{
  log_media_cast_unsupported_once(url);
}

void AirPlayRuntime::videoGetPlayInfo(double *duration, double *position,
                                      double *rate)
{
  if (duration) {
    *duration = 0.0;
  }
  if (position) {
    *position = 0.0;
  }
  if (rate) {
    *rate = 0.0;
  }
}

void AirPlayRuntime::setVolume(float, const char *, const char *)
{}

bool AirPlayRuntime::approvePairingRequest(const char *remoteName,
                                           const char *remoteDeviceId,
                                           const char *remoteModel,
                                           const char *remoteOsName,
                                           const char *remoteOsVersion,
                                           const char *remoteOsBuildVersion,
                                           const char *remoteSourceVersion,
                                           const char *)
{
  std::lock_guard<std::mutex> lock(mutex_);
  blog(LOG_INFO,
       "[obs-airplay-source] auto-approving pairing request: name='%s' id='%s' model='%s' os='%s %s (%s)' source='%s'",
       remoteName ? remoteName : "unknown",
       remoteDeviceId ? remoteDeviceId : "unknown",
       remoteModel ? remoteModel : "unknown",
       remoteOsName ? remoteOsName : "",
       remoteOsVersion ? remoteOsVersion : "",
       remoteOsBuildVersion ? remoteOsBuildVersion : "",
       remoteSourceVersion ? remoteSourceVersion : "");
  return true;
}

bool AirPlayRuntime::requestPinApproval(const char *remoteAddress,
                                        const char *pin)
{
  blog(LOG_INFO,
       "[obs-airplay-source] pin approval requested for %s (pin: %s)",
       remoteAddress ? remoteAddress : "unknown", pin ? pin : "");
  return true;
}

void AirPlayRuntime::log(int level, const char *msg)
{
  if (!msg || !msg[0]) {
    return;
  }

  if (airplay_should_suppress_runtime_log(msg)) {
    return;
  }
  blog(obs_log_level_from_airplay(level), "[obs-airplay-source] %s", msg);
}

#include "airplay-runtime.h"

#include "airplay-log-filter.h"
#include "airplay-video-format.h"

#include <obs-module.h>
#include <media-io/video-io.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
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
    candidates.push_back((std::filesystem::path(dir) / kAirPlayDllName).string());
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

} // namespace

AirPlayRuntime::AirPlayRuntime(obs_source_t *source) : source_(source) {}

AirPlayRuntime::~AirPlayRuntime()
{
  stop();
}

bool AirPlayRuntime::start(const AirPlaySettings &settings)
{
  stop();

  settings_ = settings;
  settings_.receiver_name = normalize_receiver_name(settings_.receiver_name);
  stopping_ = false;
  unsupported_media_logged_ = false;
  unsupported_audio_logged_ = false;

  if (!load_library()) {
    stopping_ = true;
    return false;
  }

  std::array<char, kAirPlayNameLength> receiver_name = {};
  strncpy_s(receiver_name.data(), receiver_name.size(),
            settings_.receiver_name.c_str(), _TRUNCATE);

  server_ = fg_server_start_(
      receiver_name.data(), static_cast<unsigned int>(settings_.raop_port),
      static_cast<unsigned int>(settings_.airplay_port), this);
  if (!server_) {
    blog(LOG_ERROR,
         "[obs-airplay-source] AirPlay receiver failed to start on ports %d/%d",
         settings_.raop_port, settings_.airplay_port);
    unload_library();
    stopping_ = true;
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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    server = server_;
    server_ = nullptr;
    stop_fn = fg_server_stop_;
    active_device_id_.clear();
    active_device_name_.clear();
  }

  if (server && stop_fn) {
    stop_fn(server);
  }

  clear_video();
  {
    std::lock_guard<std::mutex> video_lock(video_mutex_);
    reset_h264_decoder();
  }
  unload_library();
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

    fg_server_start_ = resolve_export<FgServerStartFn>(
        library,
        {"fgServerStart",
         "?fgServerStart@@YAPEAXPEBDIIPEAVIAirServerCallback@@@Z",
         "?fgServerStart@@YAPEAXQEBDIIPEAVIAirServerCallback@@@Z"});
    fg_server_stop_ = resolve_export<FgServerStopFn>(
        library, {"fgServerStop", "?fgServerStop@@YAXPEAX@Z"});
    fg_server_scale_ = resolve_export<FgServerScaleFn>(
        library, {"fgServerScale", "?fgServerScale@@YAMPEAXM@Z"});

    if (fg_server_start_ && fg_server_stop_) {
      library_ = library;
      blog(LOG_INFO, "[obs-airplay-source] loaded %s", candidate.c_str());
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
  HMODULE library = static_cast<HMODULE>(library_);
  library_ = nullptr;
  fg_server_start_ = nullptr;
  fg_server_stop_ = nullptr;
  fg_server_scale_ = nullptr;
  if (library) {
    FreeLibrary(library);
  }
}

bool AirPlayRuntime::should_accept_callback(const char *remote_device_id)
{
  if (stopping_) {
    return false;
  }
  if (!remote_device_id || !remote_device_id[0]) {
    return active_device_id_.empty();
  }
  return active_device_id_.empty() || active_device_id_ == remote_device_id;
}

void AirPlayRuntime::clear_video()
{
  if (source_) {
    obs_source_output_video(source_, nullptr);
  }
}

void AirPlayRuntime::output_airplay_video(const SFgVideoFrame *frame)
{
  if (is_encoded_h264_frame(frame)) {
    output_encoded_h264(frame);
    return;
  }

  if (!frame || !frame->data || !frame->width || !frame->height) {
    return;
  }

  const uint64_t timestamp_ns = os_gettime_ns();
  obs_source_frame obs_frame = {};
  obs_frame.format = VIDEO_FORMAT_I420;
  obs_frame.width = frame->width;
  obs_frame.height = frame->height;
  obs_frame.timestamp = timestamp_ns;
  obs_frame.full_range = false;
  video_format_get_parameters_for_format(
      airplay_colorspace_for_height(frame->height), VIDEO_RANGE_PARTIAL,
      obs_frame.format, obs_frame.color_matrix, obs_frame.color_range_min,
      obs_frame.color_range_max);

  obs_frame.data[0] = frame->data;
  obs_frame.data[1] = frame->data + frame->dataLen[0];
  obs_frame.data[2] = frame->data + frame->dataLen[0] + frame->dataLen[1];
  obs_frame.linesize[0] = frame->pitch[0];
  obs_frame.linesize[1] = frame->pitch[1];
  obs_frame.linesize[2] = frame->pitch[2];

  obs_source_output_video(source_, &obs_frame);
  width_ = frame->width;
  height_ = frame->height;
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
  if (!frame || !frame->data[0] || frame->width <= 0 || frame->height <= 0) {
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
  obs_frame.data[0] = frame->data[0];
  obs_frame.data[1] = frame->data[1];
  obs_frame.linesize[0] = static_cast<uint32_t>(frame->linesize[0]);
  obs_frame.linesize[1] = static_cast<uint32_t>(frame->linesize[1]);
  if (obs_format == VIDEO_FORMAT_I420) {
    obs_frame.data[2] = frame->data[2];
    obs_frame.linesize[2] = static_cast<uint32_t>(frame->linesize[2]);
  }

  obs_source_output_video(source_, &obs_frame);
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

bool AirPlayRuntime::output_encoded_h264(const SFgVideoFrame *frame)
{
  if (!ensure_h264_decoder()) {
    return false;
  }

  if (!logged_encoded_video_path_) {
    blog(LOG_INFO, "[obs-airplay-source] receiving encoded AirPlay H.264");
    logged_encoded_video_path_ = true;
  }

  if (av_new_packet(video_packet_, static_cast<int>(frame->dataLen[0])) < 0) {
    blog(LOG_WARNING, "[obs-airplay-source] could not allocate H.264 packet");
    return false;
  }
  memcpy(video_packet_->data, frame->data, frame->dataLen[0]);
  video_packet_->pts = frame->pts ? static_cast<int64_t>(frame->pts)
                                  : AV_NOPTS_VALUE;
  video_packet_->dts = video_packet_->pts;
  if (frame->isKey) {
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
    blog(LOG_WARNING,
         "[obs-airplay-source] could not send H.264 packet to decoder: %s",
         av_error_string(ret).c_str());
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
  if (!frame || !frame->data || !frame->dataLen || !frame->sampleRate ||
      !frame->channels) {
    return;
  }

  if (frame->bitsPerSample != 16 ||
      speaker_layout_for_channels(frame->channels) == SPEAKERS_UNKNOWN) {
    if (!unsupported_audio_logged_.exchange(true)) {
      blog(LOG_WARNING,
           "[obs-airplay-source] unsupported audio format: %u Hz, %u channels, %u bits",
           frame->sampleRate, frame->channels, frame->bitsPerSample);
    }
    return;
  }

  const uint32_t bytes_per_frame =
      static_cast<uint32_t>(frame->channels) * (frame->bitsPerSample / 8);
  if (!bytes_per_frame) {
    return;
  }

  obs_source_audio audio = {};
  audio.data[0] = frame->data;
  audio.frames = frame->dataLen / bytes_per_frame;
  audio.speakers = speaker_layout_for_channels(frame->channels);
  audio.format = AUDIO_FORMAT_16BIT;
  audio.samples_per_sec = frame->sampleRate;
  audio.timestamp = os_gettime_ns();
  obs_source_output_audio(source_, &audio);

  if (!logged_first_audio_frame_.exchange(true)) {
    blog(LOG_INFO,
         "[obs-airplay-source] output audio frame %u Hz, %u channels, %u-bit, %u frames",
         frame->sampleRate, frame->channels, frame->bitsPerSample,
         audio.frames);
    logged_first_audio_frame_ = true;
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
    if (!remoteDeviceId || active_device_id_.empty() ||
        active_device_id_ == remoteDeviceId) {
      active_device_id_.clear();
      active_device_name_.clear();
      clear = true;
    }
  }

  if (clear) {
    blog(LOG_INFO, "[obs-airplay-source] disconnected: %s",
         remoteName ? remoteName : "(unknown)");
    {
      std::lock_guard<std::mutex> video_lock(video_mutex_);
      reset_h264_decoder();
    }
    clear_video();
  }
}

void AirPlayRuntime::outputAudio(SFgAudioFrame *data, const char *,
                                 const char *remoteDeviceId)
{
  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepted = should_accept_callback(remoteDeviceId);
  }

  if (accepted) {
    output_airplay_audio(data);
  }
}

void AirPlayRuntime::outputVideo(SFgVideoFrame *data, const char *,
                                 const char *remoteDeviceId)
{
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
    output_airplay_video(data);
  }
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

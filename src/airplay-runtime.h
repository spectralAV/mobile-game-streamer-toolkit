#pragma once

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

constexpr const char *kDefaultAirPlayReceiverName = "OBS AirPlay";
constexpr int32_t kDefaultAirPlayRaopPort = 5001;
constexpr int32_t kDefaultAirPlayPort = 7001;

struct AirPlaySettings {
  bool enabled = false;
  std::string receiver_name = kDefaultAirPlayReceiverName;
  std::string dll_path;
  int32_t raop_port = kDefaultAirPlayRaopPort;
  int32_t airplay_port = kDefaultAirPlayPort;
  std::string resolution = "1440p";
  uint32_t display_width = 2560;
  uint32_t display_height = 1440;
  bool usb_tether_enabled = false;
  bool show_disconnected_frame = true;
};

constexpr int kAirPlayNameLength = 128;

struct SFgAudioFrame {
  unsigned long long pts;
  unsigned int sampleRate;
  unsigned short channels;
  unsigned short bitsPerSample;
  unsigned int dataLen;
  unsigned char *data;
};

struct SFgH264AccessUnit {
  unsigned long long pts;
  unsigned long long dts;
  unsigned int duration;
  int isKey;
  unsigned int dataLen;
  unsigned char *data;
};

struct SFgVideoFrame {
  unsigned long long pts;
  int isKey;
  unsigned int width;
  unsigned int height;
  unsigned int pitch[3];
  unsigned int dataLen[3];
  unsigned int dataTotalLen;
  unsigned char *data;
  unsigned int encodedDataLen;
};

class IAirServerCallback {
public:
  virtual void connected(const char *remoteName,
                         const char *remoteDeviceId) = 0;
  virtual void disconnected(const char *remoteName,
                            const char *remoteDeviceId) = 0;
  virtual void outputAudio(SFgAudioFrame *data, const char *remoteName,
                           const char *remoteDeviceId) = 0;
  virtual void outputH264AccessUnit(SFgH264AccessUnit *data,
                                    const char *remoteName,
                                    const char *remoteDeviceId) = 0;
  virtual void outputVideo(SFgVideoFrame *data, const char *remoteName,
                           const char *remoteDeviceId) = 0;
  virtual void videoGeometryChanged(float sourceWidth, float sourceHeight,
                                    float outputWidth, float outputHeight,
                                    const char *remoteName,
                                    const char *remoteDeviceId) = 0;
  virtual void videoSenderPausedChanged(bool paused, const char *remoteName,
                                        const char *remoteDeviceId) = 0;
  virtual void videoPlay(char *url, double volume, double startPos) = 0;
  virtual void videoGetPlayInfo(double *duration, double *position,
                                double *rate) = 0;
  virtual void setVolume(float volume, const char *remoteName,
                         const char *remoteDeviceId) = 0;
  virtual bool approvePairingRequest(
      const char *remoteName, const char *remoteDeviceId,
      const char *remoteModel, const char *remoteOsName,
      const char *remoteOsVersion, const char *remoteOsBuildVersion,
      const char *remoteSourceVersion,
      const char *pairingFingerprint) = 0;
  virtual bool requestPinApproval(const char *remoteAddress,
                                  const char *pin) = 0;
  virtual void log(int level, const char *msg) = 0;
};

class AirPlayCallbackProxy;

class AirPlayRuntime : private IAirServerCallback {
  friend class AirPlayCallbackProxy;

public:
  explicit AirPlayRuntime(obs_source_t *source);
  ~AirPlayRuntime();

  AirPlayRuntime(const AirPlayRuntime &) = delete;
  AirPlayRuntime &operator=(const AirPlayRuntime &) = delete;

  bool start(const AirPlaySettings &settings);
  void stop();

  uint32_t width() const;
  uint32_t height() const;

private:
  using FgServerStartHeadlessFn = void *(*)(const char *, unsigned int,
                                            unsigned int, IAirServerCallback *);
  using FgServerStartWithDisplayFn = void *(*)(
      const char *, unsigned int, unsigned int, IAirServerCallback *,
      const char *, unsigned int, unsigned int);
  using FgServerStartFn = void *(*)(const char *, unsigned int, unsigned int,
                                    IAirServerCallback *, const char *);
  using FgServerStartLegacyFn = void *(*)(const char *, unsigned int,
                                          unsigned int, IAirServerCallback *);
  using FgServerStopFn = void (*)(void *);
  using FgServerScaleFn = float (*)(void *, float);

  bool load_library();
  void unload_library();
  bool should_accept_callback(const char *remote_device_id);
  void clear_video();
  void output_disconnected_frame();
  void output_airplay_video(const SFgVideoFrame *frame);
  void output_airplay_audio(const SFgAudioFrame *frame);
  bool output_encoded_h264_payload(const uint8_t *payload, size_t payload_len,
                                   uint64_t pts, int is_key);
  bool ensure_h264_decoder();
  void reset_h264_decoder();
  void output_decoded_video_frame(const AVFrame *frame);
  void log_media_cast_unsupported_once(const char *url);
  void load_sps_pps_cache();
  void save_sps_pps_cache();
  std::string get_sps_pps_cache_path() const;

  void connected(const char *remoteName, const char *remoteDeviceId) override;
  void disconnected(const char *remoteName,
                    const char *remoteDeviceId) override;
  void outputAudio(SFgAudioFrame *data, const char *remoteName,
                   const char *remoteDeviceId) override;
  void outputH264AccessUnit(SFgH264AccessUnit *data, const char *remoteName,
                            const char *remoteDeviceId) override;
  void outputVideo(SFgVideoFrame *data, const char *remoteName,
                   const char *remoteDeviceId) override;
  void videoGeometryChanged(float sourceWidth, float sourceHeight,
                            float outputWidth, float outputHeight,
                            const char *remoteName,
                            const char *remoteDeviceId) override;
  void videoSenderPausedChanged(bool paused, const char *remoteName,
                                const char *remoteDeviceId) override;
  void videoPlay(char *url, double volume, double startPos) override;
  void videoGetPlayInfo(double *duration, double *position,
                        double *rate) override;
  void setVolume(float volume, const char *remoteName,
                 const char *remoteDeviceId) override;
  bool approvePairingRequest(const char *remoteName,
                             const char *remoteDeviceId,
                             const char *remoteModel,
                             const char *remoteOsName,
                             const char *remoteOsVersion,
                             const char *remoteOsBuildVersion,
                             const char *remoteSourceVersion,
                             const char *pairingFingerprint) override;
  bool requestPinApproval(const char *remoteAddress,
                          const char *pin) override;
  void log(int level, const char *msg) override;

  obs_source_t *source_ = nullptr;
  AirPlaySettings settings_;

  std::mutex mutex_;
  std::mutex video_mutex_;
  std::mutex audio_mutex_;
  uint64_t next_audio_ts_ = 0;
  uint64_t audio_frames_emitted_ = 0;
  uint64_t next_audio_log_ns_ = 0;
  std::string active_device_id_;
  std::string active_device_name_;
  std::atomic<bool> stopping_{true};
  std::atomic<bool> running_{false};
  std::atomic<bool> unsupported_media_logged_{false};
  std::atomic<bool> unsupported_audio_logged_{false};
  bool logged_first_video_frame_ = false;
  std::atomic<bool> logged_first_audio_frame_{false};
  bool logged_encoded_video_path_ = false;
  bool logged_unsupported_video_format_ = false;
  std::atomic<bool> using_h264_access_units_{false};

  std::vector<uint8_t> cached_sps_pps_;
  bool config_pending_ = false;
  std::vector<uint8_t> video_buffer_storage_;

  void *library_ = nullptr;
  void *server_ = nullptr;
  FgServerStartHeadlessFn fg_server_start_headless_ = nullptr;
  FgServerStartWithDisplayFn fg_server_start_with_display_ = nullptr;
  FgServerStartFn fg_server_start_ = nullptr;
  FgServerStartLegacyFn fg_server_start_legacy_ = nullptr;
  FgServerStopFn fg_server_stop_ = nullptr;
  FgServerScaleFn fg_server_scale_ = nullptr;
  AVCodecContext *video_decoder_ = nullptr;
  AVFrame *video_frame_ = nullptr;
  AVPacket *video_packet_ = nullptr;

  std::atomic<uint32_t> width_{1280};
  std::atomic<uint32_t> height_{720};
  std::atomic<bool> disconnected_frame_visible_{false};

  std::shared_ptr<AirPlayCallbackProxy> callback_proxy_;
  std::shared_ptr<std::atomic<bool>> last_teardown_completed_;
};

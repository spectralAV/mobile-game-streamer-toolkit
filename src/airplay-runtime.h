#pragma once

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

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

struct SFgVideoFrame {
  unsigned long long pts;
  int isKey;
  unsigned int width;
  unsigned int height;
  unsigned int pitch[3];
  unsigned int dataLen[3];
  unsigned int dataTotalLen;
  unsigned char *data;
};

class IAirServerCallback {
public:
  virtual void connected(const char *remoteName,
                         const char *remoteDeviceId) = 0;
  virtual void disconnected(const char *remoteName,
                            const char *remoteDeviceId) = 0;
  virtual void outputAudio(SFgAudioFrame *data, const char *remoteName,
                           const char *remoteDeviceId) = 0;
  virtual void outputVideo(SFgVideoFrame *data, const char *remoteName,
                           const char *remoteDeviceId) = 0;
  virtual void videoPlay(char *url, double volume, double startPos) = 0;
  virtual void videoGetPlayInfo(double *duration, double *position,
                                double *rate) = 0;
  virtual void setVolume(float volume, const char *remoteName,
                         const char *remoteDeviceId) = 0;
  virtual void log(int level, const char *msg) = 0;
};

class AirPlayRuntime : private IAirServerCallback {
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
  using FgServerStartFn = void *(*)(const char *, unsigned int, unsigned int,
                                    IAirServerCallback *);
  using FgServerStopFn = void (*)(void *);
  using FgServerScaleFn = float (*)(void *, float);

  bool load_library();
  void unload_library();
  bool should_accept_callback(const char *remote_device_id);
  void clear_video();
  void output_airplay_video(const SFgVideoFrame *frame);
  void output_airplay_audio(const SFgAudioFrame *frame);
  bool output_encoded_h264(const SFgVideoFrame *frame);
  bool ensure_h264_decoder();
  void reset_h264_decoder();
  void output_decoded_video_frame(const AVFrame *frame);
  void log_media_cast_unsupported_once(const char *url);

  void connected(const char *remoteName, const char *remoteDeviceId) override;
  void disconnected(const char *remoteName,
                    const char *remoteDeviceId) override;
  void outputAudio(SFgAudioFrame *data, const char *remoteName,
                   const char *remoteDeviceId) override;
  void outputVideo(SFgVideoFrame *data, const char *remoteName,
                   const char *remoteDeviceId) override;
  void videoPlay(char *url, double volume, double startPos) override;
  void videoGetPlayInfo(double *duration, double *position,
                        double *rate) override;
  void setVolume(float volume, const char *remoteName,
                 const char *remoteDeviceId) override;
  void log(int level, const char *msg) override;

  obs_source_t *source_ = nullptr;
  AirPlaySettings settings_;

  std::mutex mutex_;
  std::mutex video_mutex_;
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

  void *library_ = nullptr;
  void *server_ = nullptr;
  FgServerStartFn fg_server_start_ = nullptr;
  FgServerStopFn fg_server_stop_ = nullptr;
  FgServerScaleFn fg_server_scale_ = nullptr;
  AVCodecContext *video_decoder_ = nullptr;
  AVFrame *video_frame_ = nullptr;
  AVPacket *video_packet_ = nullptr;

  std::atomic<uint32_t> width_{1280};
  std::atomic<uint32_t> height_{720};
};

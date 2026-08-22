#pragma once

#include <cstdint>

// Hard ceiling for intentional A/V sync offset and device-side audio buffer.
constexpr int kMaxRealtimeDelayMs = 100;
constexpr uint64_t kMaxRealtimeDelayUs =
    static_cast<uint64_t>(kMaxRealtimeDelayMs) * 1000;
constexpr uint32_t kFirstVideoFrameTimeoutMs = 3000;

enum class LiveEdgeVideoAction {
  DecodeAndPresent,
  DecodeWithoutPresent,
  RebaseClock,
};

enum class LiveEdgeAudioAction {
  Decode,
  SkipPayload,
};

uint32_t normalize_scrcpy_scid(uint32_t value);
uint32_t scrcpy_reconnect_delay_ms(uint32_t retry_count);
bool scrcpy_first_video_frame_timed_out(uint64_t now_ns,
                                        uint64_t session_started_ns,
                                        uint64_t first_frame_at_ns);

class LiveEdgeVideoPolicy {
public:
  LiveEdgeVideoAction decide(uint64_t socket_backlog_bytes,
                             double pts_drift_ms);
  bool take_clock_rebase_request();

private:
  bool suppressing_presentation_ = false;
  bool clock_rebase_requested_ = false;
};

LiveEdgeAudioAction
live_edge_audio_action(uint64_t packet_pts_us,
                       uint64_t latest_presented_video_pts_us,
                       uint64_t socket_backlog_bytes);

uint64_t scrcpy_media_timestamp_ns(uint64_t pts_us, int target_delay_ms);

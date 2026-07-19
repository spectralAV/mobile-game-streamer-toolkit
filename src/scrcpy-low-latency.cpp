#include "scrcpy-low-latency.h"

#include <algorithm>

namespace {

constexpr uint64_t kVideoLiveEdgeBacklogBytes = 512 * 1024;
constexpr uint64_t kVideoCaughtUpBacklogBytes = 128 * 1024;
constexpr uint64_t kAudioLiveEdgeBacklogBytes = 128 * 1024;

} // namespace

uint32_t normalize_scrcpy_scid(uint32_t value)
{
  return value & 0x7fffffffu;
}

uint32_t scrcpy_reconnect_delay_ms(uint32_t retry_count)
{
  if (retry_count == 0) {
    return 0;
  }

  constexpr uint32_t kInitialDelayMs = 750;
  constexpr uint32_t kMaximumDelayMs = 60000;
  const uint32_t shift = std::min<uint32_t>(retry_count - 1, 7);
  return std::min<uint32_t>(kMaximumDelayMs, kInitialDelayMs << shift);
}

LiveEdgeVideoAction LiveEdgeVideoPolicy::decide(uint64_t socket_backlog_bytes,
                                                double pts_drift_ms)
{
  if (suppressing_presentation_) {
    // Once the receive queue is drained there is nothing left to catch up by
    // suppressing presentation. Resume and let the caller rebase its PTS clock
    // so a persistent transport offset cannot freeze the source forever.
    if (socket_backlog_bytes <= kVideoCaughtUpBacklogBytes) {
      suppressing_presentation_ = false;
      clock_rebase_requested_ = true;
    }
  } else if (socket_backlog_bytes > kVideoLiveEdgeBacklogBytes ||
             pts_drift_ms > static_cast<double>(kMaxRealtimeDelayMs)) {
    suppressing_presentation_ = true;
  }

  return suppressing_presentation_ ? LiveEdgeVideoAction::DecodeWithoutPresent
                                  : LiveEdgeVideoAction::DecodeAndPresent;
}

bool LiveEdgeVideoPolicy::take_clock_rebase_request()
{
  const bool requested = clock_rebase_requested_;
  clock_rebase_requested_ = false;
  return requested;
}

LiveEdgeAudioAction live_edge_audio_action(uint64_t packet_pts_us,
                                           uint64_t latest_presented_video_pts_us,
                                           uint64_t socket_backlog_bytes)
{
  // Only skip stale audio already received on a backlogged socket.
  // Ahead-of-video audio is kept and stamped at the live edge instead.
  if (socket_backlog_bytes <= kAudioLiveEdgeBacklogBytes ||
      latest_presented_video_pts_us == 0) {
    return LiveEdgeAudioAction::Decode;
  }
  if (packet_pts_us + kMaxRealtimeDelayUs < latest_presented_video_pts_us) {
    return LiveEdgeAudioAction::SkipPayload;
  }
  return LiveEdgeAudioAction::Decode;
}

uint64_t realtime_audio_timestamp_ns(uint64_t audio_pts_us,
                                      uint64_t latest_presented_video_pts_us,
                                      uint64_t now_ns,
                                      int target_delay_ms)
{
  const int64_t target_delay_us =
      static_cast<int64_t>(
          std::clamp(target_delay_ms, 0, kMaxRealtimeDelayMs)) *
      1000;
  // Prevent OBS from queueing seconds of future audio after video stutters.
  // Always decode; clamp scheduling to ±kMaxRealtimeDelayMs around wall clock.
  if (latest_presented_video_pts_us == 0) {
    return now_ns + static_cast<uint64_t>(target_delay_us) * 1000;
  }

  int64_t delta_us = static_cast<int64_t>(audio_pts_us) -
                         static_cast<int64_t>(latest_presented_video_pts_us) +
                     target_delay_us;
  delta_us = std::clamp(delta_us,
                        -static_cast<int64_t>(kMaxRealtimeDelayUs),
                        static_cast<int64_t>(kMaxRealtimeDelayUs));
  const int64_t ts =
      static_cast<int64_t>(now_ns) + delta_us * 1000;
  return ts > 0 ? static_cast<uint64_t>(ts) : now_ns;
}

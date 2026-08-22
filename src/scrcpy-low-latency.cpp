#include "scrcpy-low-latency.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// The Windows receive buffer used by this source is normally 64 KiB. Thresholds
// above that value never fired even while adb had seconds of queued video.
constexpr uint64_t kVideoLiveEdgeBacklogBytes = 48 * 1024;
constexpr uint64_t kVideoCaughtUpBacklogBytes = 8 * 1024;
constexpr double kVideoCaughtUpDriftMs = 50.0;
constexpr double kVideoRebaseDriftMs = 1000.0;

} // namespace

uint32_t normalize_scrcpy_scid(uint32_t value) { return value & 0x7fffffffu; }

uint32_t scrcpy_reconnect_delay_ms(uint32_t retry_count) {
  if (retry_count == 0) {
    return 0;
  }

  constexpr uint32_t kInitialDelayMs = 750;
  constexpr uint32_t kMaximumDelayMs = 60000;
  const uint32_t shift = std::min<uint32_t>(retry_count - 1, 7);
  return std::min<uint32_t>(kMaximumDelayMs, kInitialDelayMs << shift);
}

bool scrcpy_first_video_frame_timed_out(uint64_t now_ns,
                                        uint64_t session_started_ns,
                                        uint64_t first_frame_at_ns) {
  if (session_started_ns == 0 || first_frame_at_ns != 0 ||
      now_ns < session_started_ns) {
    return false;
  }

  constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
  return now_ns - session_started_ns >=
         static_cast<uint64_t>(kFirstVideoFrameTimeoutMs) *
             kNanosecondsPerMillisecond;
}

LiveEdgeVideoAction LiveEdgeVideoPolicy::decide(uint64_t socket_backlog_bytes,
                                                double pts_drift_ms) {
  if (std::abs(pts_drift_ms) > kVideoRebaseDriftMs) {
    suppressing_presentation_ = false;
    clock_rebase_requested_ = true;
    return LiveEdgeVideoAction::RebaseClock;
  }

  if (suppressing_presentation_) {
    if (socket_backlog_bytes <= kVideoCaughtUpBacklogBytes &&
        pts_drift_ms <= kVideoCaughtUpDriftMs) {
      suppressing_presentation_ = false;
      clock_rebase_requested_ = true;
    }
  } else if (pts_drift_ms > static_cast<double>(kMaxRealtimeDelayMs) ||
             (socket_backlog_bytes > kVideoLiveEdgeBacklogBytes &&
              pts_drift_ms > kVideoCaughtUpDriftMs)) {
    suppressing_presentation_ = true;
  }

  return suppressing_presentation_ ? LiveEdgeVideoAction::DecodeWithoutPresent
                                   : LiveEdgeVideoAction::DecodeAndPresent;
}

bool LiveEdgeVideoPolicy::take_clock_rebase_request() {
  const bool requested = clock_rebase_requested_;
  clock_rebase_requested_ = false;
  return requested;
}

LiveEdgeAudioAction
live_edge_audio_action(uint64_t packet_pts_us,
                       uint64_t latest_presented_video_pts_us,
                       uint64_t socket_backlog_bytes) {
  (void)socket_backlog_bytes;
  if (latest_presented_video_pts_us == 0) {
    return LiveEdgeAudioAction::Decode;
  }
  // Stale audio can remain queued behind video even after FIONREAD falls below
  // one receive-buffer window. PTS is the authoritative cross-stream signal.
  if (latest_presented_video_pts_us > packet_pts_us &&
      latest_presented_video_pts_us - packet_pts_us > kMaxRealtimeDelayUs) {
    return LiveEdgeAudioAction::SkipPayload;
  }
  // Keep audio from running away while video waits for the first keyframe of a
  // new capture session or recovers from a decoder error.
  if (packet_pts_us > latest_presented_video_pts_us &&
      packet_pts_us - latest_presented_video_pts_us > kMaxRealtimeDelayUs) {
    return LiveEdgeAudioAction::SkipPayload;
  }
  return LiveEdgeAudioAction::Decode;
}

uint64_t scrcpy_media_timestamp_ns(uint64_t pts_us, int target_delay_ms) {
  constexpr uint64_t kNanosecondsPerMicrosecond = 1000;
  constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
  const uint64_t delay_ns = static_cast<uint64_t>(std::clamp(
                                target_delay_ms, 0, kMaxRealtimeDelayMs)) *
                            kNanosecondsPerMillisecond;
  if (pts_us > (std::numeric_limits<uint64_t>::max() - delay_ns) /
                   kNanosecondsPerMicrosecond) {
    return std::numeric_limits<uint64_t>::max();
  }
  return pts_us * kNanosecondsPerMicrosecond + delay_ns;
}

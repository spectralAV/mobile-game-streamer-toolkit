#include "scrcpy-low-latency.h"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char *description)
{
  if (condition) {
    return;
  }
  std::cerr << description << " failed\n";
  std::exit(1);
}

} // namespace

int main()
{
  expect(normalize_scrcpy_scid(0xffffffffu) == 0x7fffffffu,
         "scrcpy session IDs stay within signed 31-bit range");
  expect(normalize_scrcpy_scid(0x12345678u) == 0x12345678u,
         "valid scrcpy session IDs are unchanged");

  expect(scrcpy_reconnect_delay_ms(0) == 0,
         "no reconnect attempt has no delay");
  expect(scrcpy_reconnect_delay_ms(1) == 750,
         "the first reconnect is prompt");
  expect(scrcpy_reconnect_delay_ms(4) == 6000,
         "reconnect delay backs off exponentially");
  expect(scrcpy_reconnect_delay_ms(20) == 60000,
         "reconnect delay is capped at one minute");

  LiveEdgeVideoPolicy video_policy;
  expect(video_policy.decide(0, 0.0) ==
             LiveEdgeVideoAction::DecodeAndPresent,
         "caught-up video decodes and presents");
  expect(video_policy.decide(64 * 1024, 80.0) ==
             LiveEdgeVideoAction::DecodeAndPresent,
         "a transient scheduling spike still presents");
  expect(video_policy.decide(0, 150.0) ==
             LiveEdgeVideoAction::DecodeWithoutPresent,
         "measured video lag suppresses stale presentation");
  expect(video_policy.decide(0, 60.0) ==
              LiveEdgeVideoAction::DecodeAndPresent,
         "drained video resumes despite a persistent clock offset");
  expect(video_policy.take_clock_rebase_request(),
         "resuming presentation requests a PTS clock rebase");
  expect(!video_policy.take_clock_rebase_request(),
         "the PTS clock rebase request is consumed once");

  LiveEdgeVideoPolicy socket_policy;
  expect(socket_policy.decide(1024 * 1024, 0.0) ==
             LiveEdgeVideoAction::DecodeWithoutPresent,
         "large socket backlog suppresses stale presentation");
  expect(socket_policy.decide(0, 0.0) ==
             LiveEdgeVideoAction::DecodeAndPresent,
         "empty socket backlog resumes presentation");

  expect(live_edge_audio_action(260 * 1000, 200 * 1000, 0) ==
             LiveEdgeAudioAction::Decode,
         "audio ahead of presented video is kept for smooth playback");
  expect(live_edge_audio_action(5000 * 1000, 200 * 1000, 0) ==
             LiveEdgeAudioAction::Decode,
         "far-ahead audio is kept; timestamps clamp to live edge");
  expect(live_edge_audio_action(0, 200 * 1000, 256 * 1024) ==
             LiveEdgeAudioAction::SkipPayload,
         "stale audio is skipped when the socket is backlogged");
  expect(live_edge_audio_action(150 * 1000, 200 * 1000, 256 * 1024) ==
             LiveEdgeAudioAction::Decode,
         "audio near the presented video edge is kept");

  const uint64_t now_ns = 1'000'000'000;
  expect(realtime_audio_timestamp_ns(200 * 1000, 100 * 1000, now_ns, 0) ==
              now_ns + static_cast<uint64_t>(100 * 1000) * 1000,
         "in-sync audio stays within the realtime delay window");
  expect(realtime_audio_timestamp_ns(10'000 * 1000, 100 * 1000, now_ns, 0) ==
              now_ns + static_cast<uint64_t>(kMaxRealtimeDelayMs) * 1'000'000,
         "far-ahead audio is clamped to the max delay ceiling");
  expect(realtime_audio_timestamp_ns(0, 10'000 * 1000, now_ns, 0) ==
              now_ns - static_cast<uint64_t>(kMaxRealtimeDelayMs) * 1'000'000,
         "far-behind audio is clamped to the max delay ceiling");
  expect(realtime_audio_timestamp_ns(100 * 1000, 100 * 1000, now_ns, 50) ==
             now_ns + 50'000'000,
         "configured audio buffering delays OBS scheduling");
  expect(realtime_audio_timestamp_ns(100 * 1000, 0, now_ns, 50) ==
             now_ns + 50'000'000,
         "audio buffering applies before the first video timestamp");

  return 0;
}

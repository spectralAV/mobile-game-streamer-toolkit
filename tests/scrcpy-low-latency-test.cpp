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

  constexpr uint64_t session_started_ns = 1'000'000'000;
  expect(!scrcpy_first_video_frame_timed_out(
             session_started_ns +
                 static_cast<uint64_t>(kFirstVideoFrameTimeoutMs - 1) *
                     1'000'000,
             session_started_ns, 0),
         "the first-frame watchdog allows normal decoder startup");
  expect(scrcpy_first_video_frame_timed_out(
             session_started_ns +
                 static_cast<uint64_t>(kFirstVideoFrameTimeoutMs) * 1'000'000,
             session_started_ns, 0),
         "a connected session without a first frame times out");
  expect(!scrcpy_first_video_frame_timed_out(
             session_started_ns +
                 static_cast<uint64_t>(kFirstVideoFrameTimeoutMs) * 2'000'000,
             session_started_ns, session_started_ns + 50'000'000),
         "an emitted first frame disables the startup watchdog");

  LiveEdgeVideoPolicy video_policy;
  expect(video_policy.decide(0, 0.0) ==
             LiveEdgeVideoAction::DecodeAndPresent,
         "caught-up video decodes and presents");
  expect(video_policy.decide(4 * 1024, 80.0) ==
             LiveEdgeVideoAction::DecodeAndPresent,
         "a transient scheduling spike still presents");
  expect(video_policy.decide(0, 150.0) ==
             LiveEdgeVideoAction::DecodeWithoutPresent,
         "measured video lag suppresses stale presentation");
  expect(video_policy.decide(0, 40.0) ==
              LiveEdgeVideoAction::DecodeAndPresent,
         "drained video resumes after PTS catches up");
  expect(video_policy.take_clock_rebase_request(),
         "resuming presentation requests a PTS clock rebase");
  expect(!video_policy.take_clock_rebase_request(),
         "the PTS clock rebase request is consumed once");

  LiveEdgeVideoPolicy socket_policy;
  expect(socket_policy.decide(1024 * 1024, 80.0) ==
             LiveEdgeVideoAction::DecodeWithoutPresent,
         "large socket backlog plus PTS lag suppresses stale presentation");
  expect(socket_policy.decide(0, 0.0) ==
             LiveEdgeVideoAction::DecodeAndPresent,
         "empty socket backlog resumes presentation");

  LiveEdgeVideoPolicy discontinuity_policy;
  expect(discontinuity_policy.decide(0, 1500.0) ==
             LiveEdgeVideoAction::RestartStream,
         "multi-second stale video requests a clean live-edge restart");
  expect(discontinuity_policy.decide(0, -1500.0) ==
             LiveEdgeVideoAction::RestartStream,
         "a forward PTS discontinuity requests a clean restart");

  expect(live_edge_audio_action(260 * 1000, 200 * 1000, 0) ==
             LiveEdgeAudioAction::Decode,
         "audio ahead of presented video is kept for smooth playback");
  expect(live_edge_audio_action(5000 * 1000, 200 * 1000, 0) ==
             LiveEdgeAudioAction::Decode,
         "far-ahead audio is kept on the shared device PTS timeline");
  expect(live_edge_audio_action(0, 200 * 1000, 0) ==
             LiveEdgeAudioAction::SkipPayload,
         "stale audio is skipped using cross-stream PTS");
  expect(live_edge_audio_action(150 * 1000, 200 * 1000, 256 * 1024) ==
             LiveEdgeAudioAction::Decode,
         "audio near the presented video edge is kept");

  expect(scrcpy_media_timestamp_ns(200 * 1000, 0) == 200'000'000,
         "device microsecond PTS converts to the shared OBS nanosecond timeline");
  expect(scrcpy_media_timestamp_ns(200 * 1000, 50) == 250'000'000,
         "configured audio buffering offsets the shared media timeline");
  expect(scrcpy_media_timestamp_ns(200 * 1000, 500) == 300'000'000,
         "configured audio buffering is clamped to the realtime ceiling");

  return 0;
}

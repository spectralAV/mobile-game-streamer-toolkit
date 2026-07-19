#include "airplay-log-filter.h"

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
  expect(airplay_should_suppress_runtime_log(
             "dispatch H264 frame to decoder, len=70771 nalus=1"),
         "per-frame H.264 dispatch logs are suppressed");
  expect(airplay_should_suppress_runtime_log(
             "video_process frame_type=1 len=70771"),
         "per-frame video_process logs are suppressed");
  expect(airplay_should_suppress_runtime_log(
             "returned from H264 frame decoder dispatch"),
         "per-frame decoder return logs are suppressed");
  expect(airplay_should_suppress_runtime_log(
             "raop_rtp_mirror_thread_time send time 48 bytes, port = 49226"),
         "mirror timing heartbeat logs are suppressed");

  expect(!airplay_should_suppress_runtime_log(
             "receiver 'OBS AirPlay' started on RAOP port 5001 and AirPlay port 7001"),
         "receiver startup logs are preserved");
  expect(!airplay_should_suppress_runtime_log("connected: iPhone"),
         "device connection logs are preserved");
  expect(!airplay_should_suppress_runtime_log(
             "output audio frame 44100 Hz, 2 channels, 16-bit, 480 frames"),
         "first audio output logs are preserved");

  return 0;
}

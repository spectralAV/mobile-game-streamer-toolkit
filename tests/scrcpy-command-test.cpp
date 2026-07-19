#include "scrcpy-command.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect_equal(const std::vector<std::string> &actual,
                  const std::vector<std::string> &expected,
                  const char *description)
{
  if (actual == expected) {
    return;
  }

  std::cerr << description << " failed\nExpected:";
  for (const std::string &arg : expected) {
    std::cerr << " [" << arg << "]";
  }
  std::cerr << "\nActual:";
  for (const std::string &arg : actual) {
    std::cerr << " [" << arg << "]";
  }
  std::cerr << '\n';
  std::exit(1);
}

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
  ScrcpySettings tcpip;
  tcpip.connection_mode = ConnectionMode::Tcpip;
  tcpip.tcpip_address = "10.160.216.99:40661";

  expect_equal(build_adb_connect_args(tcpip),
               {"connect", "10.160.216.99:40661"},
               "TCP/IP sessions connect the configured address");
  expect_equal(build_adb_selector_args(tcpip),
               {"-s", "10.160.216.99:40661"},
               "TCP/IP sessions select the configured address");

  tcpip.serial = "stale-usb-serial";
  expect_equal(build_adb_selector_args(tcpip),
               {"-s", "10.160.216.99:40661"},
               "TCP/IP address takes precedence over a stale serial");

  ScrcpySettings usb;
  usb.serial = "usb-device";
  expect_equal(build_adb_connect_args(usb), {},
               "USB sessions do not run adb connect");
  expect_equal(build_adb_selector_args(usb), {"-s", "usb-device"},
               "USB sessions select the configured serial");

  expect(merge_realtime_video_codec_options("").empty(),
         "empty codec options are passed through unchanged");
  expect(merge_realtime_video_codec_options("bitrate-mode:int=1") ==
             "bitrate-mode:int=1",
         "custom codec options are passed through unchanged");
  expect(merge_realtime_video_codec_options("i-frame-interval:float=2") ==
             "i-frame-interval:float=2",
         "explicit iframe settings are passed through unchanged");
  expect(normalize_video_codec_options("Device default (no extra options)")
             .empty(),
         "UI preset labels must not be sent as MediaFormat options");
  expect(normalize_video_codec_options("Plugin default (auto 1s keyframes)")
             .empty(),
         "legacy UI preset labels must not be sent as MediaFormat options");
  expect(normalize_audio_codec_options("None").empty(),
         "audio None label must not be sent as MediaFormat options");

  return 0;
}

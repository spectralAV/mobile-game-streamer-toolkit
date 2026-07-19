#include "airplay-video-format.h"

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
  expect(airplay_video_format_from_av_pixel_format(AV_PIX_FMT_YUV420P) ==
             AirPlayVideoFormat::I420,
         "standard 8-bit YUV 4:2:0 frames map to OBS I420");
  expect(airplay_video_format_from_av_pixel_format(AV_PIX_FMT_NV12) ==
             AirPlayVideoFormat::NV12,
         "NV12 frames map to OBS NV12");
  expect(airplay_video_format_from_av_pixel_format(AV_PIX_FMT_YUVJ420P) ==
             AirPlayVideoFormat::I420,
         "full-range AirPlay YUVJ 4:2:0 frames map to OBS I420");
  expect(airplay_pixel_format_implies_full_range(AV_PIX_FMT_YUVJ420P),
         "YUVJ 4:2:0 frames imply full-range color");

  return 0;
}

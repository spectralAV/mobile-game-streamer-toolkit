#pragma once

extern "C" {
#include <libavutil/pixfmt.h>
}

enum class AirPlayVideoFormat {
  Unsupported,
  I420,
  NV12,
};

AirPlayVideoFormat
airplay_video_format_from_av_pixel_format(AVPixelFormat format);

bool airplay_pixel_format_implies_full_range(AVPixelFormat format);

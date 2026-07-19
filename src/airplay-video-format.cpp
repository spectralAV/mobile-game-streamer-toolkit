#include "airplay-video-format.h"

AirPlayVideoFormat
airplay_video_format_from_av_pixel_format(AVPixelFormat format)
{
  switch (format) {
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
    return AirPlayVideoFormat::I420;
  case AV_PIX_FMT_NV12:
    return AirPlayVideoFormat::NV12;
  default:
    return AirPlayVideoFormat::Unsupported;
  }
}

bool airplay_pixel_format_implies_full_range(AVPixelFormat format)
{
  return format == AV_PIX_FMT_YUVJ420P;
}

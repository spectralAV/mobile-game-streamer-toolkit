#include "airplay-log-filter.h"

#include <cstring>

namespace {

bool contains(const char *message, const char *needle)
{
  return std::strstr(message, needle) != nullptr;
}

} // namespace

bool airplay_should_suppress_runtime_log(const char *message)
{
  if (!message || !message[0]) {
    return false;
  }

  if (contains(message, "dispatch H264 frame to decoder") ||
      contains(message, "video_process frame_type=") ||
      contains(message, "decodeH264Data returned") ||
      contains(message, "returned from H264 frame decoder dispatch") ||
      contains(message, "raop_rtp_mirror_thread_time send") ||
      contains(message, "raop_rtp_mirror_thread_time receive")) {
    return true;
  }

  return false;
}

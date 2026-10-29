#include "scrcpy-disconnected-frame.h"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char *description) {
  if (condition) {
    return;
  }
  std::cerr << description << " failed\n";
  std::exit(1);
}

size_t count_accent_pixels(const ScrcpyDisconnectedFrame &frame) {
  size_t count = 0;
  for (size_t offset = 0; offset + 3 < frame.bgra.size(); offset += 4) {
    if (frame.bgra[offset] == 55 && frame.bgra[offset + 1] == 72 &&
        frame.bgra[offset + 2] == 232 && frame.bgra[offset + 3] == 255) {
      ++count;
    }
  }
  return count;
}

void verify_frame(uint32_t width, uint32_t height) {
  const ScrcpyDisconnectedFrame frame =
      make_scrcpy_disconnected_frame(width, height);
  expect(frame.width == width, "frame keeps the requested canvas width");
  expect(frame.height == height, "frame keeps the requested canvas height");
  expect(frame.stride == width * 4, "frame exposes a packed BGRA stride");
  expect(frame.bgra.size() == static_cast<size_t>(width) * height * 4,
         "frame storage matches the canvas dimensions");
  expect(count_accent_pixels(frame) > static_cast<size_t>(width + height),
         "frame contains a visible disconnected accent");
}

} // namespace

int main() {
  verify_frame(320, 180);
  verify_frame(180, 320);
  verify_frame(1280, 720);
  verify_frame(1920, 1080);
  verify_frame(2560, 1440);
  verify_frame(1440, 2560);

  expect(make_scrcpy_disconnected_frame(0, 720).bgra.empty(),
         "zero-width canvases are rejected");
  expect(make_scrcpy_disconnected_frame(9000, 720).bgra.empty(),
         "oversized canvases are rejected");
  return 0;
}

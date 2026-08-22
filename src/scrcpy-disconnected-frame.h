#pragma once

#include <cstdint>
#include <vector>

struct ScrcpyDisconnectedFrame {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  std::vector<uint8_t> bgra;
};

ScrcpyDisconnectedFrame make_scrcpy_disconnected_frame(uint32_t width,
                                                        uint32_t height);

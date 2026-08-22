#include "scrcpy-disconnected-frame.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace {

constexpr uint32_t kMaxDimension = 8192;
constexpr uint64_t kMaxPixels = 64ull * 1024ull * 1024ull;

struct Color {
  uint8_t blue;
  uint8_t green;
  uint8_t red;
  uint8_t alpha = 255;
};

bool valid_dimensions(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > kMaxDimension ||
      height > kMaxDimension) {
    return false;
  }
  return static_cast<uint64_t>(width) * height <= kMaxPixels;
}

void set_pixel(ScrcpyDisconnectedFrame &frame, int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= static_cast<int>(frame.width) ||
      y >= static_cast<int>(frame.height)) {
    return;
  }
  const size_t offset = static_cast<size_t>(y) * frame.stride +
                        static_cast<size_t>(x) * 4;
  frame.bgra[offset] = color.blue;
  frame.bgra[offset + 1] = color.green;
  frame.bgra[offset + 2] = color.red;
  frame.bgra[offset + 3] = color.alpha;
}

void fill_rect(ScrcpyDisconnectedFrame &frame, int left, int top, int right,
               int bottom, Color color) {
  left = std::clamp(left, 0, static_cast<int>(frame.width));
  right = std::clamp(right, 0, static_cast<int>(frame.width));
  top = std::clamp(top, 0, static_cast<int>(frame.height));
  bottom = std::clamp(bottom, 0, static_cast<int>(frame.height));
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      set_pixel(frame, x, y, color);
    }
  }
}

void draw_outline(ScrcpyDisconnectedFrame &frame, int left, int top, int right,
                  int bottom, int thickness, Color color) {
  fill_rect(frame, left, top, right, top + thickness, color);
  fill_rect(frame, left, bottom - thickness, right, bottom, color);
  fill_rect(frame, left, top, left + thickness, bottom, color);
  fill_rect(frame, right - thickness, top, right, bottom, color);
}

void draw_thick_line(ScrcpyDisconnectedFrame &frame, int x0, int y0, int x1,
                     int y1, int thickness, Color color) {
  const int dx = std::abs(x1 - x0);
  const int dy = std::abs(y1 - y0);
  const int steps = std::max(dx, dy);
  if (steps == 0) {
    fill_rect(frame, x0, y0, x0 + thickness, y0 + thickness, color);
    return;
  }

  const int radius = std::max(0, thickness / 2);
  for (int step = 0; step <= steps; ++step) {
    const int x = x0 + (x1 - x0) * step / steps;
    const int y = y0 + (y1 - y0) * step / steps;
    fill_rect(frame, x - radius, y - radius, x + radius + 1, y + radius + 1,
              color);
  }
}

std::array<uint8_t, 7> glyph(char character) {
  switch (character) {
  case 'C':
    return {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e};
  case 'D':
    return {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
  case 'E':
    return {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
  case 'I':
    return {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f};
  case 'N':
    return {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11};
  case 'O':
    return {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
  case 'S':
    return {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
  case 'T':
    return {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
  case 'V':
    return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04};
  default:
    return {};
  }
}

void draw_centered_text(ScrcpyDisconnectedFrame &frame, std::string_view text,
                        int center_y, Color color) {
  constexpr int glyph_width = 5;
  constexpr int glyph_height = 7;
  constexpr int glyph_gap = 1;
  const int text_units = static_cast<int>(text.size()) *
                             (glyph_width + glyph_gap) -
                         glyph_gap;
  const int width_scale = std::max(
      1, static_cast<int>(frame.width * 78 / 100) / std::max(1, text_units));
  const int height_scale = std::max(
      1, static_cast<int>(frame.height * 11 / 100) / glyph_height);
  const int scale = std::max(1, std::min(width_scale, height_scale));
  const int text_width = text_units * scale;
  const int text_height = glyph_height * scale;
  int cursor_x = (static_cast<int>(frame.width) - text_width) / 2;
  const int top = center_y - text_height / 2;

  for (char character : text) {
    const auto rows = glyph(character);
    for (int row = 0; row < glyph_height; ++row) {
      for (int column = 0; column < glyph_width; ++column) {
        if ((rows[row] & (1u << (glyph_width - column - 1))) == 0) {
          continue;
        }
        fill_rect(frame, cursor_x + column * scale, top + row * scale,
                  cursor_x + (column + 1) * scale,
                  top + (row + 1) * scale, color);
      }
    }
    cursor_x += (glyph_width + glyph_gap) * scale;
  }
}

} // namespace

ScrcpyDisconnectedFrame make_scrcpy_disconnected_frame(uint32_t width,
                                                        uint32_t height) {
  ScrcpyDisconnectedFrame frame;
  if (!valid_dimensions(width, height)) {
    return frame;
  }

  frame.width = width;
  frame.height = height;
  frame.stride = width * 4;
  frame.bgra.resize(static_cast<size_t>(frame.stride) * height);

  for (uint32_t y = 0; y < height; ++y) {
    const int vertical = static_cast<int>(y * 10 / height);
    for (uint32_t x = 0; x < width; ++x) {
      const int horizontal = static_cast<int>(x * 6 / width);
      const uint8_t shade = static_cast<uint8_t>(15 + vertical + horizontal);
      set_pixel(frame, static_cast<int>(x), static_cast<int>(y),
                {static_cast<uint8_t>(shade + 6), shade, shade, 255});
    }
  }

  const int min_dimension = static_cast<int>(std::min(width, height));
  const int margin = std::max(3, min_dimension / 24);
  const int border = std::max(2, min_dimension / 180);
  const Color accent = {55, 72, 232, 255};
  const Color panel = {28, 24, 24, 255};
  const Color text = {232, 238, 245, 255};

  fill_rect(frame, margin, margin, static_cast<int>(width) - margin,
            static_cast<int>(height) - margin, panel);
  draw_outline(frame, margin, margin, static_cast<int>(width) - margin,
               static_cast<int>(height) - margin, border, accent);

  const int icon_height = std::clamp(min_dimension * 30 / 100, 28,
                                     static_cast<int>(height) * 36 / 100);
  const int icon_width = std::max(16, icon_height * 56 / 100);
  const int icon_left = (static_cast<int>(width) - icon_width) / 2;
  const int icon_top = static_cast<int>(height) * 20 / 100;
  const int icon_right = icon_left + icon_width;
  const int icon_bottom = icon_top + icon_height;
  const int icon_border = std::max(2, min_dimension / 120);
  draw_outline(frame, icon_left, icon_top, icon_right, icon_bottom, icon_border,
               text);
  fill_rect(frame, icon_left + icon_width * 36 / 100,
            icon_bottom - icon_border * 3, icon_left + icon_width * 64 / 100,
            icon_bottom - icon_border, text);
  draw_thick_line(frame, icon_left - icon_border, icon_bottom - icon_border,
                  icon_right + icon_border, icon_top + icon_border,
                  icon_border * 2, accent);

  const int text_center_y = std::max(icon_bottom + min_dimension * 12 / 100,
                                     static_cast<int>(height) * 69 / 100);
  draw_centered_text(frame, "DEVICE DISCONNECTED", text_center_y, text);
  return frame;
}

#pragma once
// 128x64 monochrome framebuffer in SSD1306 page layout (8 pages of 128
// columns, one byte = 8 vertical pixels, LSB on top). The same buffer is sent
// to the panel and to the web preview, so the preview is pixel-identical.
#include <cstdint>
#include <cstring>
#include <string>

namespace hc {

class Framebuffer {
 public:
  static constexpr int W = 128;
  static constexpr int H = 64;
  static constexpr int kBytes = W * H / 8;

  void clear() { std::memset(buf_, 0, sizeof(buf_)); }
  void setPixel(int x, int y, bool on = true);
  bool pixel(int x, int y) const;
  void hline(int x, int y, int w, bool on = true);
  void vline(int x, int y, int h, bool on = true);
  void rect(int x, int y, int w, int h, bool on = true);
  void fillRect(int x, int y, int w, int h, bool on = true);
  void invertRect(int x, int y, int w, int h);
  // Text with the built-in 5x7 font (UTF-8, German umlauts, degree sign).
  // Returns the x position after the text.
  int text(int x, int y, const std::string& s, int scale = 1, bool on = true);
  static int textWidth(const std::string& s, int scale = 1);
  // Truncates with '.' so the text fits into maxWidth pixels.
  int textFit(int x, int y, const std::string& s, int maxWidth, int scale = 1);
  int textRight(int xRight, int y, const std::string& s, int scale = 1);
  void bar(int x, int y, int w, int h, float fraction);

  const uint8_t* data() const { return buf_; }
  uint8_t* data() { return buf_; }
  // Rotates by 180 degrees in place (used when the hardware cannot).
  void rotate180();
  uint32_t checksum() const;

 private:
  uint8_t buf_[kBytes] = {0};
};

// Decodes the next glyph index from UTF-8 text; returns bytes consumed.
int nextGlyph(const char* s, uint8_t& glyph);
const uint8_t* glyphData(uint8_t glyph);  // 5 column bytes

}  // namespace hc

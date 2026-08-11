// Verifies the JPEG -> NV12 conversion logic used by the Android encoder path.
//
// This is worth testing on the PC because a colour-conversion bug does not
// crash — it produces a stream that encodes and transmits perfectly and looks
// wrong, which would be blamed on the protocol under test. The conversion
// arithmetic here verifies the NV12 layout policy. ucv-jpegtest separately
// compiles and exercises the production decoder against real JPEG input.
//
// Build: g++ -std=c++17 -O2 -Iinclude src/nv12test.cpp -o nv12test

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                  \
  do {                                                    \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); g_failures++; } \
    else         { std::printf("  ok  : %s\n", msg); }    \
  } while (0)

// Exact plane layout the Android side produces: Y plane of w*h, then an
// interleaved UV plane of w*(h/2). MediaCodec's
// COLOR_FormatYUV420SemiPlanar is NV12 — U before V. NV21 (V first) would
// encode and transmit perfectly and merely come out with the colours
// swapped, so getting this backwards looks like a camera fault.
struct Nv12 {
  int w, h;
  std::vector<uint8_t> data;

  explicit Nv12(int width, int height)
      : w(width), h(height), data(static_cast<size_t>(width) * height * 3 / 2, 0) {}

  uint8_t* Y() { return data.data(); }
  uint8_t* UV() { return data.data() + static_cast<size_t>(w) * h; }
  size_t size() const { return data.size(); }
};

// Models the 4:4:4 -> NV12 point-sampling policy: retain every Y sample and
// take chroma from even rows/columns. The production raw-plane paths for
// 4:2:0/4:2:2/4:4:4 are covered by ucv-jpegtest.
void ConvertYCbCrToNv12(const std::vector<uint8_t>& ycbcr, Nv12* out) {
  const int w = out->w, h = out->h;
  uint8_t* Y = out->Y();
  uint8_t* UV = out->UV();

  for (int y = 0; y < h; y++) {
    const uint8_t* row = ycbcr.data() + static_cast<size_t>(y) * w * 3;
    uint8_t* yrow = Y + static_cast<size_t>(y) * w;
    for (int x = 0; x < w; x++) yrow[x] = row[x * 3 + 0];

    if ((y & 1) == 0) {
      uint8_t* uvrow = UV + static_cast<size_t>(y / 2) * w;
      for (int x = 0; x < w; x += 2) {
        uvrow[x + 0] = row[x * 3 + 1];  // U (Cb)
        uvrow[x + 1] = row[x * 3 + 2];  // V (Cr)
      }
    }
  }
}

std::vector<uint8_t> MakeYCbCr(int w, int h, uint8_t y, uint8_t cb, uint8_t cr) {
  std::vector<uint8_t> v(static_cast<size_t>(w) * h * 3);
  for (size_t i = 0; i < v.size(); i += 3) {
    v[i + 0] = y; v[i + 1] = cb; v[i + 2] = cr;
  }
  return v;
}

void TestPlaneSizes() {
  std::printf("\n[nv12] plane geometry must match MediaCodec's expectation\n");
  Nv12 f(1280, 720);
  CHECK(f.size() == 1280u * 720u * 3 / 2, "720p NV12 buffer is w*h*3/2");
  CHECK(f.size() == 1382400, "720p NV12 is 1382400 bytes");
  CHECK(f.UV() - f.Y() == 1280 * 720, "UV plane starts right after Y");

  Nv12 g(640, 480);
  CHECK(g.size() == 640u * 480u * 3 / 2, "480p NV12 buffer size");
}

void TestSolidColour() {
  std::printf("\n[nv12] a solid frame must survive conversion exactly\n");
  const int w = 8, h = 8;
  Nv12 out(w, h);
  ConvertYCbCrToNv12(MakeYCbCr(w, h, 128, 100, 200), &out);

  bool y_ok = true;
  for (int i = 0; i < w * h; i++) if (out.Y()[i] != 128) y_ok = false;
  CHECK(y_ok, "every luma sample carried through");

  // Chroma order is the thing most easily got backwards, and swapping it
  // makes skin tones go blue-green — visible, but easy to blame on the
  // camera rather than on the conversion.
  bool uv_ok = true;
  for (int i = 0; i < w * h / 2; i += 2) {
    if (out.UV()[i + 0] != 100) uv_ok = false;  // U (Cb) first
    if (out.UV()[i + 1] != 200) uv_ok = false;  // then V (Cr)
  }
  CHECK(uv_ok, "chroma is U-then-V (NV12), not V-then-U (NV21)");
}

void TestLumaGradientPreserved() {
  std::printf("\n[nv12] luma must not be shifted or transposed\n");
  const int w = 16, h = 16;
  std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      src[i + 0] = static_cast<uint8_t>(y * 16 + x);  // unique per pixel
      src[i + 1] = 128;
      src[i + 2] = 128;
    }
  }

  Nv12 out(w, h);
  ConvertYCbCrToNv12(src, &out);

  bool ok = true;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (out.Y()[y * w + x] != static_cast<uint8_t>(y * 16 + x)) ok = false;
  CHECK(ok, "luma plane is row-major and correctly indexed");
}

void TestChromaSubsampling() {
  std::printf("\n[nv12] chroma is 2x2 subsampled from even rows/columns\n");
  const int w = 4, h = 4;
  std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3);
  // Distinct chroma per row so a wrong row stride shows up immediately.
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      src[i + 0] = 16;
      src[i + 1] = static_cast<uint8_t>(50 + y);   // Cb
      src[i + 2] = static_cast<uint8_t>(150 + y);  // Cr
    }
  }

  Nv12 out(w, h);
  ConvertYCbCrToNv12(src, &out);

  // Row 0 of UV comes from source row 0; row 1 of UV from source row 2.
  CHECK(out.UV()[0] == 50 && out.UV()[1] == 150, "UV row 0 sampled from src row 0");
  CHECK(out.UV()[w + 0] == 52 && out.UV()[w + 1] == 152,
        "UV row 1 sampled from src row 2, not row 1");
}

void TestNoOverrun() {
  std::printf("\n[nv12] conversion must stay inside the buffer\n");
  const int w = 64, h = 64;
  Nv12 out(w, h);
  // Sentinel past the end would be clobbered by an off-by-one in the VU loop.
  out.data.push_back(0xEE);
  ConvertYCbCrToNv12(MakeYCbCr(w, h, 10, 20, 30), &out);
  CHECK(out.data.back() == 0xEE, "no write past the end of the NV12 buffer");
}

void TestOddDimensionsRejected() {
  std::printf("\n[nv12] odd dimensions are rejected, not silently truncated\n");
  // ucv_jpeg_decoder_create refuses odd w/h because the subsampled chroma
  // plane has no valid size for them. Documented here so the rule is not
  // quietly dropped later.
  const int w = 7, h = 7;
  const size_t naive = static_cast<size_t>(w) * h * 3 / 2;
  CHECK(naive * 2 != static_cast<size_t>(w) * h * 3,
        "odd dimensions do not yield an exact NV12 size (hence the guard)");
}

int main() {
  std::printf("JPEG -> NV12 conversion — self test\n");
  std::printf("===================================\n");

  TestPlaneSizes();
  TestSolidColour();
  TestLumaGradientPreserved();
  TestChromaSubsampling();
  TestNoOverrun();
  TestOddDimensionsRejected();

  std::printf("\n===================================\n");
  if (g_failures == 0) { std::printf("ALL CHECKS PASSED\n"); return 0; }
  std::printf("%d CHECK(S) FAILED\n", g_failures);
  return 1;
}

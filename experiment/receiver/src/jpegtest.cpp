// Exercises the Android MJPEG -> NV12 decoder on the host. This intentionally
// links the production ucv_encoder.c with UCV_JPEG_ONLY rather than copying its
// implementation into a test-specific helper.

#include "ucv_encoder.h"

#include <cstdio>

extern "C" {
#include <jpeglib.h>
}

#include <cstdint>
#include <cstdlib>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); g_failures++; } \
    else         { std::printf("  ok  : %s\n", msg); }              \
  } while (0)

static std::vector<uint8_t> MakeGrayJpeg(int width, int height,
                                         uint8_t value) {
  jpeg_compress_struct cinfo{};
  jpeg_error_mgr error{};
  cinfo.err = jpeg_std_error(&error);
  jpeg_create_compress(&cinfo);

  unsigned char* encoded = nullptr;
  unsigned long encoded_size = 0;
  jpeg_mem_dest(&cinfo, &encoded, &encoded_size);
  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 90, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  std::vector<uint8_t> row(static_cast<size_t>(width) * 3, value);
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW rows[] = {row.data()};
    jpeg_write_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_compress(&cinfo);

  std::vector<uint8_t> result(encoded, encoded + encoded_size);
  std::free(encoded);
  jpeg_destroy_compress(&cinfo);
  return result;
}

static uint64_t Checksum(const uint8_t* data, size_t size) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

static void TestCreateGuards() {
  std::printf("\n[jpeg] decoder creation guards\n");
  CHECK(ucv_jpeg_decoder_create(0, 8) == nullptr,
        "zero width is rejected");
  CHECK(ucv_jpeg_decoder_create(7, 8) == nullptr,
        "odd width is rejected for NV12");
  CHECK(ucv_jpeg_decoder_create(8, 7) == nullptr,
        "odd height is rejected for NV12");
}

static void TestPersistentDecodeAndRecovery() {
  constexpr int kWidth = 16;
  constexpr int kHeight = 16;
  const auto valid = MakeGrayJpeg(kWidth, kHeight, 128);
  const auto wrong_size = MakeGrayJpeg(kWidth * 2, kHeight, 128);
  const std::vector<uint8_t> corrupt = {
      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

  ucv_jpeg_decoder_t* decoder =
      ucv_jpeg_decoder_create(kWidth, kHeight);
  CHECK(decoder != nullptr, "persistent decoder is created");
  if (!decoder) return;

  const uint8_t* nv12 = nullptr;
  size_t nv12_size = 0;
  int rc = ucv_jpeg_decode_to_nv12(decoder, valid.data(), valid.size(),
                                   &nv12, &nv12_size);
  CHECK(rc == 0, "first valid JPEG decodes");
  CHECK(nv12 != nullptr, "decoder returns an NV12 buffer");
  CHECK(nv12_size == static_cast<size_t>(kWidth) * kHeight * 3 / 2,
        "NV12 buffer has w*h*3/2 bytes");

  const uint8_t* first_buffer = nv12;
  const uint64_t first_checksum = nv12 ? Checksum(nv12, nv12_size) : 0;
  bool repeated_ok = rc == 0;
  for (int i = 0; i < 250 && repeated_ok; ++i) {
    rc = ucv_jpeg_decode_to_nv12(decoder, valid.data(), valid.size(),
                                 &nv12, &nv12_size);
    repeated_ok = rc == 0 && nv12 == first_buffer &&
                  Checksum(nv12, nv12_size) == first_checksum;
  }
  CHECK(repeated_ok, "250 decodes reuse one stable buffer and output");

  rc = ucv_jpeg_decode_to_nv12(decoder, corrupt.data(), corrupt.size(),
                               &nv12, &nv12_size);
  CHECK(rc < 0, "corrupt JPEG is rejected without terminating the process");
  rc = ucv_jpeg_decode_to_nv12(decoder, valid.data(), valid.size(),
                               &nv12, &nv12_size);
  CHECK(rc == 0 && nv12 == first_buffer,
        "decoder recovers after a fatal JPEG parse error");

  rc = ucv_jpeg_decode_to_nv12(decoder, wrong_size.data(), wrong_size.size(),
                               &nv12, &nv12_size);
  CHECK(rc < 0, "unexpected frame dimensions are rejected");
  rc = ucv_jpeg_decode_to_nv12(decoder, valid.data(), valid.size(),
                               &nv12, &nv12_size);
  CHECK(rc == 0 && Checksum(nv12, nv12_size) == first_checksum,
        "decoder recovers after an aborted size mismatch");

  ucv_jpeg_decoder_destroy(decoder);
}

int main() {
  std::printf("Persistent JPEG decoder - host smoke test\n");
  std::printf("=========================================\n");
  TestCreateGuards();
  TestPersistentDecodeAndRecovery();

  std::printf("\n=========================================\n");
  if (g_failures == 0) {
    std::printf("ALL CHECKS PASSED\n");
    return 0;
  }
  std::printf("%d CHECK(S) FAILED\n", g_failures);
  return 1;
}

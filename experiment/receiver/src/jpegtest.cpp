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

static std::vector<uint8_t> MakeSolidJpeg(int width, int height,
                                          uint8_t red, uint8_t green,
                                          uint8_t blue, int y_h_samp = 2,
                                          int y_v_samp = 2) {
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
  cinfo.comp_info[0].h_samp_factor = y_h_samp;
  cinfo.comp_info[0].v_samp_factor = y_v_samp;
  cinfo.comp_info[1].h_samp_factor = 1;
  cinfo.comp_info[1].v_samp_factor = 1;
  cinfo.comp_info[2].h_samp_factor = 1;
  cinfo.comp_info[2].v_samp_factor = 1;
  jpeg_set_quality(&cinfo, 90, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
  for (int x = 0; x < width; ++x) {
    row[static_cast<size_t>(x) * 3 + 0] = red;
    row[static_cast<size_t>(x) * 3 + 1] = green;
    row[static_cast<size_t>(x) * 3 + 2] = blue;
  }
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

static std::vector<uint8_t> MakeGrayscaleJpeg(int width, int height,
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
  cinfo.input_components = 1;
  cinfo.in_color_space = JCS_GRAYSCALE;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 90, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  std::vector<uint8_t> row(static_cast<size_t>(width), value);
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

static void TestRawSubsamplingLayouts() {
  std::printf("\n[jpeg] raw Y/Cb/Cr planes must map to NV12 for camera layouts\n");
  constexpr int kWidth = 18;   // exercises right-edge DCT padding
  constexpr int kHeight = 10;  // exercises a partial final iMCU row
  ucv_jpeg_decoder_t* decoder =
      ucv_jpeg_decoder_create(kWidth, kHeight);
  CHECK(decoder != nullptr, "raw-layout decoder is created");
  if (!decoder) return;

  struct Layout { int h, v; const char* name; };
  const Layout layouts[] = {
      {2, 2, "4:2:0"}, {2, 1, "4:2:2"}, {1, 1, "4:4:4"}};
  const uint8_t* first_buffer = nullptr;
  for (const auto& layout : layouts) {
    const auto jpeg = MakeSolidJpeg(kWidth, kHeight, 220, 40, 30,
                                    layout.h, layout.v);
    const uint8_t* nv12 = nullptr;
    size_t size = 0;
    const int rc = ucv_jpeg_decode_to_nv12(
        decoder, jpeg.data(), jpeg.size(), &nv12, &size);
    char message[128];
    std::snprintf(message, sizeof(message), "%s raw planes decode", layout.name);
    CHECK(rc == 0, message);
    if (rc != 0) continue;
    if (!first_buffer) first_buffer = nv12;
    std::snprintf(message, sizeof(message), "%s reuses the NV12 buffer", layout.name);
    CHECK(nv12 == first_buffer, message);

    const uint8_t* uv = nv12 + static_cast<size_t>(kWidth) * kHeight;
    uint64_t u_sum = 0, v_sum = 0;
    for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight / 2; i += 2) {
      u_sum += uv[i];
      v_sum += uv[i + 1];
    }
    std::snprintf(message, sizeof(message), "%s keeps U before V", layout.name);
    CHECK(v_sum > u_sum + 40u * (kWidth * kHeight / 4), message);
  }

  const auto gray = MakeGrayscaleJpeg(kWidth, kHeight, 96);
  const uint8_t* nv12 = nullptr;
  size_t size = 0;
  int rc = ucv_jpeg_decode_to_nv12(
      decoder, gray.data(), gray.size(), &nv12, &size);
  CHECK(rc == 0, "grayscale JPEG decodes through the raw path");
  bool neutral_uv = rc == 0;
  if (neutral_uv) {
    const uint8_t* uv = nv12 + static_cast<size_t>(kWidth) * kHeight;
    for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight / 2; ++i)
      if (uv[i] != 128) neutral_uv = false;
  }
  CHECK(neutral_uv, "grayscale JPEG produces neutral NV12 chroma");
  ucv_jpeg_decoder_destroy(decoder);
}

static void TestCallerOwnedOutput() {
  std::printf("\n[jpeg] decoder writes directly into caller-owned NV12\n");
  constexpr int kWidth = 18;
  constexpr int kHeight = 10;
  constexpr size_t kGuard = 16;
  constexpr uint8_t kSentinel = 0xA5;
  const size_t expected_size =
      static_cast<size_t>(kWidth) * kHeight * 3 / 2;
  const auto jpeg = MakeSolidJpeg(kWidth, kHeight, 40, 160, 210);
  ucv_jpeg_decoder_t* decoder =
      ucv_jpeg_decoder_create(kWidth, kHeight);
  CHECK(decoder != nullptr, "external-output decoder is created");
  if (!decoder) return;

  std::vector<uint8_t> guarded(expected_size + kGuard * 2, kSentinel);
  uint8_t* destination = guarded.data() + kGuard;
  size_t output_size = 0;
  int rc = ucv_jpeg_decode_into_nv12(
      decoder, jpeg.data(), jpeg.size(), destination, expected_size - 1,
      &output_size);
  CHECK(rc < 0, "undersized destination is rejected before decode");
  bool untouched = true;
  for (uint8_t value : guarded)
    if (value != kSentinel) untouched = false;
  CHECK(untouched, "undersized destination remains untouched");

  rc = ucv_jpeg_decode_into_nv12(
      decoder, jpeg.data(), jpeg.size(), destination, expected_size,
      &output_size);
  CHECK(rc == 0, "JPEG decodes into caller-owned storage");
  CHECK(output_size == expected_size, "external output reports exact NV12 size");
  bool guards_ok = true;
  for (size_t i = 0; i < kGuard; ++i) {
    if (guarded[i] != kSentinel ||
        guarded[kGuard + expected_size + i] != kSentinel)
      guards_ok = false;
  }
  CHECK(guards_ok, "external decode stays inside destination bounds");

  const uint64_t external_checksum = Checksum(destination, expected_size);
  const uint8_t* internal = nullptr;
  size_t internal_size = 0;
  rc = ucv_jpeg_decode_to_nv12(decoder, jpeg.data(), jpeg.size(),
                               &internal, &internal_size);
  CHECK(rc == 0 && internal_size == expected_size,
        "compatibility decoder remains available");
  CHECK(rc == 0 && Checksum(internal, internal_size) == external_checksum,
        "external and compatibility paths produce identical NV12");

  ucv_jpeg_decoder_destroy(decoder);
}

static void TestPersistentDecodeAndRecovery() {
  constexpr int kWidth = 18;
  constexpr int kHeight = 10;
  const auto valid = MakeSolidJpeg(kWidth, kHeight, 128, 128, 128);
  const auto wrong_size = MakeSolidJpeg(kWidth * 2, kHeight, 128, 128, 128);
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
  TestRawSubsamplingLayouts();
  TestCallerOwnedOutput();
  TestPersistentDecodeAndRecovery();

  std::printf("\n=========================================\n");
  if (g_failures == 0) {
    std::printf("ALL CHECKS PASSED\n");
    return 0;
  }
  std::printf("%d CHECK(S) FAILED\n", g_failures);
  return 1;
}

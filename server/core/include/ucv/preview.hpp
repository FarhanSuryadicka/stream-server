// Live camera preview, mirroring the Python dashboard's pipeline exactly.
//
// The receiver is the ONLY listener on the video port — a second socket
// competing for the phone's packets would corrupt the measurement it exists to
// produce. So the receiver mirrors each complete frame to a localhost UDP port
// AFTER reception, and this class listens there. The preview therefore cannot
// affect a single number in the run.
//
// H.264 protocols are decoded by FFmpeg (h264 -> mjpeg on stdio). MJPEG arrives
// already as JPEG and is passed through untouched, which is why its preview
// works with no FFmpeg installed at all.

#ifndef UCV_PREVIEW_HPP
#define UCV_PREVIEW_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ucv {

enum class PreviewState { Idle, Waiting, Live, Stopped, Error };

const char* to_string(PreviewState state);

class PreviewPipeline {
 public:
  PreviewPipeline();
  ~PreviewPipeline();

  PreviewPipeline(const PreviewPipeline&) = delete;
  PreviewPipeline& operator=(const PreviewPipeline&) = delete;

  // Binds a localhost UDP port and, for H.264 protocols, spawns FFmpeg.
  // Returns the port to pass to the receiver as --preview-port, or 0 on
  // failure (the reason is in error()). A preview failure must never stop a
  // run, so callers treat 0 as "no preview" rather than as fatal.
  int start(const std::string& protocol);
  void stop();

  PreviewState state() const;
  std::string error() const;
  std::uint64_t frame_count() const;

  // Most recent JPEG, empty until one arrives. Copied out under the lock
  // because the decoder thread replaces it continuously.
  std::vector<std::uint8_t> latest_jpeg() const;

  // Invoked on the decoder thread each time a new frame is ready.
  void set_on_frame(std::function<void()> callback);

 private:
  struct Impl;
  Impl* impl_;
};

// True when FFmpeg can be found; the UI uses this to explain a missing preview
// rather than showing an empty black box with no reason.
bool ffmpeg_available(std::string* path_out = nullptr);

}  // namespace ucv

#endif  // UCV_PREVIEW_HPP

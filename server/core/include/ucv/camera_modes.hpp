// Reads the MJPEG modes the attached UVC camera actually advertises.
//
// Enumerating the camera rather than offering a fixed list matters: a mode the
// camera does not support fails at START with an error that looks like a
// network fault, and a high mode it *does* advertise may still stall over
// isochronous USB. The list is the camera's own descriptor, nothing invented.
//
// Uses the shared wire contract in experiment/receiver/include/ucv_wire.h, so
// the control packets here cannot drift from what the phone parses.

#ifndef UCV_CAMERA_MODES_HPP
#define UCV_CAMERA_MODES_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ucv {

// One resolution with every FPS the camera offers for it, highest first.
struct CameraResolution {
  int width = 0;
  int height = 0;
  std::vector<int> fps;   // descending

  // "1280x720@30" using the best FPS — what --mode expects.
  std::string best_mode() const;
  std::string label() const;   // "1280 x 720"
};

struct CameraModeQuery {
  bool ok = false;
  std::string error;
  std::vector<CameraResolution> resolutions;
};

// Asks the phone's control agent (UDP, default port 8200) to enumerate modes.
// Blocking, bounded by the per-probe timeout; intended to be called off the UI
// thread.
CameraModeQuery query_camera_modes(const std::string& phone_ip,
                                   int control_port = 8200,
                                   int timeout_ms = 600);

}  // namespace ucv

#endif  // UCV_CAMERA_MODES_HPP

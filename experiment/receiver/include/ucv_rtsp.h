#ifndef UCV_RTSP_H
#define UCV_RTSP_H

#include "ucv_net.h"

#include <string>

namespace ucv {

struct RtspResponse {
  int status_code = 0;
  int cseq = -1;
  std::string headers;
  std::string body;

  std::string Header(const std::string& name) const;
};

// Parses one complete RTSP response, including a Content-Length body when
// present. Exposed so response framing remains regression-tested independently
// of a phone and camera.
bool ParseRtspResponse(const std::string& raw, RtspResponse* out);

class RtspClient {
 public:
  ~RtspClient();

  // Opens a UDP-unicast RTSP session and leaves the TCP control connection
  // alive while RTP is received on client_rtp_port.
  bool Start(const std::string& phone_ip, int rtsp_port, int client_rtp_port,
             int timeout_ms);
  bool Teardown(int timeout_ms = 2000);
  void Close();

  bool active() const { return active_; }
  const std::string& session() const { return session_; }
  const std::string& last_error() const { return last_error_; }

 private:
  bool Request(const std::string& method, const std::string& uri,
               const std::string& extra_headers, int timeout_ms,
               RtspResponse* response);
  bool ReadResponse(int timeout_ms, RtspResponse* response);
  bool Fail(const std::string& message);

  TcpClient tcp_;
  std::string base_uri_;
  std::string session_;
  std::string receive_buffer_;
  std::string last_error_;
  int cseq_ = 0;
  bool active_ = false;
};

}  // namespace ucv

#endif  // UCV_RTSP_H

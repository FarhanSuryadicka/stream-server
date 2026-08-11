#include "ucv_rtsp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

namespace ucv {
namespace {

std::string Trim(const std::string& value) {
  size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first])))
    first++;
  size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1])))
    last--;
  return value.substr(first, last - first);
}

bool EqualHeaderName(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i])))
      return false;
  }
  return true;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return value;
}

}  // namespace

std::string RtspResponse::Header(const std::string& name) const {
  size_t line = headers.find("\r\n");  // skip the status line
  if (line == std::string::npos) return {};
  line += 2;
  while (line < headers.size()) {
    const size_t end = headers.find("\r\n", line);
    const size_t line_end = end == std::string::npos ? headers.size() : end;
    const size_t colon = headers.find(':', line);
    if (colon != std::string::npos && colon < line_end) {
      const std::string key = headers.substr(line, colon - line);
      if (EqualHeaderName(key, name))
        return Trim(headers.substr(colon + 1, line_end - colon - 1));
    }
    if (end == std::string::npos) break;
    line = end + 2;
  }
  return {};
}

bool ParseRtspResponse(const std::string& raw, RtspResponse* out) {
  if (!out) return false;
  const size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) return false;

  RtspResponse parsed;
  parsed.headers = raw.substr(0, header_end);
  if (std::sscanf(parsed.headers.c_str(), "RTSP/1.0 %d",
                  &parsed.status_code) != 1)
    return false;

  const std::string cseq = parsed.Header("CSeq");
  if (cseq.empty()) return false;
  try {
    parsed.cseq = std::stoi(cseq);
  } catch (...) {
    return false;
  }

  size_t content_length = 0;
  const std::string length = parsed.Header("Content-Length");
  if (!length.empty()) {
    try {
      content_length = static_cast<size_t>(std::stoull(length));
    } catch (...) {
      return false;
    }
  }
  const size_t body_start = header_end + 4;
  if (content_length > 1024 * 1024 ||
      raw.size() < body_start + content_length)
    return false;
  parsed.body = raw.substr(body_start, content_length);
  *out = std::move(parsed);
  return true;
}

RtspClient::~RtspClient() { Close(); }

bool RtspClient::Fail(const std::string& message) {
  last_error_ = message;
  active_ = false;
  tcp_.Close();
  return false;
}

bool RtspClient::ReadResponse(int timeout_ms, RtspResponse* response) {
  for (;;) {
    const size_t header_end = receive_buffer_.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      size_t content_length = 0;
      RtspResponse headers_only;
      headers_only.headers = receive_buffer_.substr(0, header_end);
      const std::string length = headers_only.Header("Content-Length");
      if (!length.empty()) {
        try {
          content_length = static_cast<size_t>(std::stoull(length));
        } catch (...) {
          return false;
        }
      }
      if (content_length > 1024 * 1024) return false;
      const size_t total = header_end + 4 + content_length;
      if (receive_buffer_.size() >= total) {
        const std::string one = receive_buffer_.substr(0, total);
        receive_buffer_.erase(0, total);
        return ParseRtspResponse(one, response);
      }
    }

    char buf[4096];
    const int n = tcp_.RecvTimeout(buf, sizeof(buf), timeout_ms);
    if (n <= 0) return false;
    receive_buffer_.append(buf, static_cast<size_t>(n));
    if (receive_buffer_.size() > 1024 * 1024) return false;
  }
}

bool RtspClient::Request(const std::string& method, const std::string& uri,
                         const std::string& extra_headers, int timeout_ms,
                         RtspResponse* response) {
  const int expected_cseq = ++cseq_;
  const std::string request = method + " " + uri + " RTSP/1.0\r\n" +
      "CSeq: " + std::to_string(expected_cseq) + "\r\n" +
      "User-Agent: ucv-receiver\r\n" + extra_headers + "\r\n";
  if (!tcp_.SendAll(request.data(), request.size()))
    return Fail(method + ": send failed");
  if (!ReadResponse(timeout_ms, response))
    return Fail(method + ": response timeout or malformed response");
  if (response->cseq != expected_cseq)
    return Fail(method + ": response CSeq mismatch");
  if (response->status_code != 200)
    return Fail(method + ": server returned " +
                std::to_string(response->status_code));
  return true;
}

bool RtspClient::Start(const std::string& phone_ip, int rtsp_port,
                       int client_rtp_port, int timeout_ms) {
  Close();
  last_error_.clear();
  if (phone_ip.empty() || rtsp_port <= 0 || rtsp_port > 65535 ||
      client_rtp_port <= 0 || client_rtp_port >= 65535)
    return Fail("invalid RTSP address or RTP port");
  if (!tcp_.Connect(phone_ip, rtsp_port, timeout_ms))
    return Fail("connect failed");

  base_uri_ = "rtsp://" + phone_ip + ":" + std::to_string(rtsp_port) + "/ucv";
  RtspResponse response;
  if (!Request("OPTIONS", base_uri_, "", timeout_ms, &response)) return false;
  if (!Request("DESCRIBE", base_uri_, "Accept: application/sdp\r\n",
               timeout_ms, &response))
    return false;
  const std::string sdp = Lower(response.body);
  if (sdp.find("m=video") == std::string::npos ||
      sdp.find("h264/90000") == std::string::npos)
    return Fail("DESCRIBE: SDP has no H.264 video track");

  const std::string track_uri = base_uri_ + "/track0";
  const std::string transport =
      "Transport: RTP/AVP;unicast;client_port=" +
      std::to_string(client_rtp_port) + "-" +
      std::to_string(client_rtp_port + 1) + "\r\n";
  if (!Request("SETUP", track_uri, transport, timeout_ms, &response))
    return false;
  session_ = response.Header("Session");
  const size_t semicolon = session_.find(';');
  if (semicolon != std::string::npos) session_.resize(semicolon);
  session_ = Trim(session_);
  if (session_.empty()) return Fail("SETUP: response has no Session header");

  if (!Request("PLAY", base_uri_, "Session: " + session_ + "\r\n",
               timeout_ms, &response))
    return false;
  active_ = true;
  return true;
}

bool RtspClient::Teardown(int timeout_ms) {
  if (!active_) {
    Close();
    return true;
  }
  RtspResponse response;
  const bool ok = Request("TEARDOWN", base_uri_,
                          "Session: " + session_ + "\r\n",
                          timeout_ms, &response);
  Close();
  return ok;
}

void RtspClient::Close() {
  tcp_.Close();
  base_uri_.clear();
  session_.clear();
  receive_buffer_.clear();
  cseq_ = 0;
  active_ = false;
}

}  // namespace ucv

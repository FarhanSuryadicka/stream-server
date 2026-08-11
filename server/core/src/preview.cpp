#include "ucv/preview.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ucv {

const char* to_string(PreviewState state) {
  switch (state) {
    case PreviewState::Idle:    return "idle";
    case PreviewState::Waiting: return "waiting";
    case PreviewState::Live:    return "live";
    case PreviewState::Stopped: return "stopped";
    case PreviewState::Error:   return "error";
  }
  return "idle";
}

namespace {

// Must match PreviewHeader in experiment/receiver/src/main.cpp and the
// PREVIEW_HEADER struct in the Python dashboard. Three copies of one layout is
// unfortunate but unavoidable across C++/C++/Python; the magics below make a
// mismatch fail loudly instead of producing garbage frames.
#pragma pack(push, 1)
struct PreviewHeader {
  std::uint32_t magic;
  std::uint32_t frame_seq;
  std::uint32_t frame_bytes;
  std::uint16_t fragment_index;
  std::uint16_t fragment_count;
};
#pragma pack(pop)

constexpr std::uint32_t kMagicH264 = 0x31565055;   // "UPV1"
constexpr std::uint32_t kMagicJpeg = 0x314A5055;   // "UPJ1"

struct Assembly {
  std::uint32_t size = 0;
  std::uint16_t count = 0;
  std::map<std::uint16_t, std::vector<std::uint8_t>> parts;
  std::chrono::steady_clock::time_point at;
};

}  // namespace

bool ffmpeg_available(std::string* path_out) {
  // PATH first, then the location the runbook documents for this rig.
#ifdef _WIN32
  const char* names[] = {"ffmpeg.exe"};
  const char* fallback = "C:\\Tools\\ffmpeg\\bin\\ffmpeg.exe";
#else
  const char* names[] = {"ffmpeg"};
  const char* fallback = "/usr/bin/ffmpeg";
#endif
  const char* path_env = std::getenv("PATH");
  if (path_env) {
    std::string paths(path_env);
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::size_t start = 0;
    while (start <= paths.size()) {
      const std::size_t end = paths.find(sep, start);
      const std::string dir =
          paths.substr(start, end == std::string::npos ? std::string::npos
                                                       : end - start);
      if (!dir.empty()) {
        for (const char* n : names) {
          const std::filesystem::path candidate =
              std::filesystem::path(dir) / n;
          std::error_code ec;
          if (std::filesystem::is_regular_file(candidate, ec)) {
            if (path_out) *path_out = candidate.string();
            return true;
          }
        }
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  std::error_code ec;
  if (std::filesystem::is_regular_file(fallback, ec)) {
    if (path_out) *path_out = fallback;
    return true;
  }
  return false;
}

struct PreviewPipeline::Impl {
  mutable std::mutex lock;
  PreviewState state = PreviewState::Idle;
  std::string error;
  std::vector<std::uint8_t> latest;
  std::uint64_t frames = 0;
  std::function<void()> on_frame;

  std::atomic<bool> quit{false};
  std::thread receiver_thread;
  std::thread jpeg_thread;

#ifdef _WIN32
  SOCKET sock = INVALID_SOCKET;
  PROCESS_INFORMATION ffmpeg{};
  HANDLE ff_stdin = nullptr;
  HANDLE ff_stdout = nullptr;
  bool have_ffmpeg = false;
#else
  int sock = -1;
  pid_t ffmpeg = -1;
  int ff_stdin = -1;
  int ff_stdout = -1;
#endif

  void set_state(PreviewState s) {
    std::lock_guard<std::mutex> g(lock);
    if (state != PreviewState::Error) state = s;
  }

  void set_error(const std::string& message) {
    std::lock_guard<std::mutex> g(lock);
    state = PreviewState::Error;
    error = message;
  }

  void publish(std::vector<std::uint8_t> jpeg) {
    std::function<void()> cb;
    {
      std::lock_guard<std::mutex> g(lock);
      latest = std::move(jpeg);
      frames++;
      if (state != PreviewState::Error) state = PreviewState::Live;
      cb = on_frame;
    }
    if (cb) cb();
  }
};

PreviewPipeline::PreviewPipeline() : impl_(new Impl) {}

PreviewPipeline::~PreviewPipeline() {
  stop();
  delete impl_;
}

void PreviewPipeline::set_on_frame(std::function<void()> callback) {
  std::lock_guard<std::mutex> g(impl_->lock);
  impl_->on_frame = std::move(callback);
}

PreviewState PreviewPipeline::state() const {
  std::lock_guard<std::mutex> g(impl_->lock);
  return impl_->state;
}

std::string PreviewPipeline::error() const {
  std::lock_guard<std::mutex> g(impl_->lock);
  return impl_->error;
}

std::uint64_t PreviewPipeline::frame_count() const {
  std::lock_guard<std::mutex> g(impl_->lock);
  return impl_->frames;
}

std::vector<std::uint8_t> PreviewPipeline::latest_jpeg() const {
  std::lock_guard<std::mutex> g(impl_->lock);
  return impl_->latest;
}

int PreviewPipeline::start(const std::string& protocol) {
  stop();
  {
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->error.clear();
    impl_->frames = 0;
    impl_->latest.clear();
    impl_->state = PreviewState::Waiting;
  }

#ifdef _WIN32
  impl_->sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (impl_->sock == INVALID_SOCKET) {
    impl_->set_error("cannot open preview socket");
    return 0;
  }
#else
  impl_->sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (impl_->sock < 0) {
    impl_->set_error("cannot open preview socket");
    return 0;
  }
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;                                   // any free port
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);   // localhost only
  if (::bind(impl_->sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    impl_->set_error("cannot bind preview socket");
    stop();
    return 0;
  }
  sockaddr_in bound{};
#ifdef _WIN32
  int len = sizeof(bound);
#else
  socklen_t len = sizeof(bound);
#endif
  ::getsockname(impl_->sock, reinterpret_cast<sockaddr*>(&bound), &len);
  const int port = ntohs(bound.sin_port);

  // A short receive timeout keeps the reader loop responsive to stop().
#ifdef _WIN32
  DWORD tmo = 500;
  ::setsockopt(impl_->sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tmo), sizeof(tmo));
#else
  timeval tmo{0, 500000};
  ::setsockopt(impl_->sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
#endif

  // MJPEG already carries JPEG, so it needs no decoder at all — which is why
  // its preview works on a machine with no FFmpeg.
  const bool needs_ffmpeg = (protocol != "mjpeg");
  if (needs_ffmpeg) {
    std::string ffmpeg_path;
    if (!ffmpeg_available(&ffmpeg_path)) {
      impl_->set_error(
          "FFmpeg tidak ditemukan; install FFmpeg atau tambahkan ke PATH");
      // The socket stays bound: the receiver still gets a valid --preview-port
      // and the run proceeds normally, just without a picture.
      return port;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE in_read = nullptr, in_write = nullptr;
    HANDLE out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 0) ||
        !CreatePipe(&out_read, &out_write, &sa, 0)) {
      impl_->set_error("cannot create FFmpeg pipes");
      return port;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    // Identical arguments to the Python dashboard, so both previews show the
    // same thing and neither is "the good one".
    std::string cmd = "\"" + ffmpeg_path +
        "\" -hide_banner -loglevel error -flags low_delay -probesize 32"
        " -analyzeduration 0 -f h264 -i pipe:0 -an"
        " -vf scale='min(960,iw)':-2"
        " -f image2pipe -c:v mjpeg -q:v 5 pipe:1";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&impl_->ffmpeg, sizeof(impl_->ffmpeg));
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &impl_->ffmpeg)) {
      impl_->set_error("cannot start FFmpeg");
      CloseHandle(in_read); CloseHandle(in_write);
      CloseHandle(out_read); CloseHandle(out_write);
      return port;
    }
    CloseHandle(in_read);
    CloseHandle(out_write);
    impl_->ff_stdin = in_write;
    impl_->ff_stdout = out_read;
    impl_->have_ffmpeg = true;
#else
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
      impl_->set_error("cannot create FFmpeg pipes");
      return port;
    }
    const pid_t pid = fork();
    if (pid == 0) {
      dup2(in_pipe[0], STDIN_FILENO);
      dup2(out_pipe[1], STDOUT_FILENO);
      close(in_pipe[1]);
      close(out_pipe[0]);
      execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "error",
             "-flags", "low_delay", "-probesize", "32", "-analyzeduration", "0",
             "-f", "h264", "-i", "pipe:0", "-an",
             "-vf", "scale='min(960,iw)':-2",
             "-f", "image2pipe", "-c:v", "mjpeg", "-q:v", "5", "pipe:1",
             nullptr);
      _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    impl_->ffmpeg = pid;
    impl_->ff_stdin = in_pipe[1];
    impl_->ff_stdout = out_pipe[0];
#endif

    // Pull JPEGs out of FFmpeg by scanning for SOI/EOI markers.
    impl_->jpeg_thread = std::thread([this] {
      std::vector<std::uint8_t> buffer;
      std::uint8_t chunk[65536];
      while (!impl_->quit) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(impl_->ff_stdout, chunk, sizeof(chunk), &n, nullptr) || n == 0)
          break;
#else
        const ssize_t n = ::read(impl_->ff_stdout, chunk, sizeof(chunk));
        if (n <= 0) break;
#endif
        buffer.insert(buffer.end(), chunk, chunk + n);
        // Emit every complete JPEG in the buffer, keeping any partial tail.
        for (;;) {
          if (buffer.size() < 4) break;
          std::size_t soi = std::string::npos;
          for (std::size_t i = 0; i + 1 < buffer.size(); ++i) {
            if (buffer[i] == 0xFF && buffer[i + 1] == 0xD8) { soi = i; break; }
          }
          if (soi == std::string::npos) { buffer.clear(); break; }
          std::size_t eoi = std::string::npos;
          for (std::size_t i = soi + 2; i + 1 < buffer.size(); ++i) {
            if (buffer[i] == 0xFF && buffer[i + 1] == 0xD9) { eoi = i + 1; break; }
          }
          if (eoi == std::string::npos) {
            if (soi > 0) buffer.erase(buffer.begin(), buffer.begin() + soi);
            break;
          }
          impl_->publish({buffer.begin() + soi, buffer.begin() + eoi + 1});
          buffer.erase(buffer.begin(), buffer.begin() + eoi + 1);
        }
      }
    });
  }

  // Reassemble mirrored frames and either publish (JPEG) or feed FFmpeg (H.264).
  impl_->receiver_thread = std::thread([this] {
    std::map<std::uint32_t, Assembly> pending;
    std::vector<std::uint8_t> packet(70000);
    while (!impl_->quit) {
#ifdef _WIN32
      const int n = ::recv(impl_->sock, reinterpret_cast<char*>(packet.data()),
                           static_cast<int>(packet.size()), 0);
#else
      const ssize_t n = ::recv(impl_->sock, packet.data(), packet.size(), 0);
#endif
      if (n <= static_cast<int>(sizeof(PreviewHeader))) {
        // Drop half-assembled frames whose remaining fragments never arrived,
        // so a lossy moment cannot leak memory for the rest of the run.
        const auto now = std::chrono::steady_clock::now();
        for (auto it = pending.begin(); it != pending.end();) {
          if (now - it->second.at > std::chrono::seconds(2))
            it = pending.erase(it);
          else
            ++it;
        }
        continue;
      }
      PreviewHeader h{};
      std::memcpy(&h, packet.data(), sizeof(h));
      if ((h.magic != kMagicH264 && h.magic != kMagicJpeg) ||
          h.fragment_count == 0 || h.fragment_index >= h.fragment_count ||
          h.frame_bytes > 32u * 1024u * 1024u)
        continue;

      Assembly& item = pending[h.frame_seq];
      if (item.count == 0) {
        item.size = h.frame_bytes;
        item.count = h.fragment_count;
        item.at = std::chrono::steady_clock::now();
      }
      if (item.size != h.frame_bytes || item.count != h.fragment_count) {
        pending.erase(h.frame_seq);
        continue;
      }
      item.parts[h.fragment_index].assign(
          packet.begin() + sizeof(PreviewHeader), packet.begin() + n);
      if (item.parts.size() != item.count) continue;

      std::vector<std::uint8_t> frame;
      frame.reserve(item.size);
      for (std::uint16_t i = 0; i < item.count; ++i) {
        const auto part = item.parts.find(i);
        if (part != item.parts.end())
          frame.insert(frame.end(), part->second.begin(), part->second.end());
      }
      const std::uint32_t magic = h.magic;
      pending.erase(h.frame_seq);
      if (frame.size() != item.size) continue;

      if (magic == kMagicJpeg) {
        // Only publish a JPEG that is actually complete; a truncated one paints
        // as a grey band and looks like a camera fault.
        if (frame.size() > 4 && frame[0] == 0xFF && frame[1] == 0xD8 &&
            frame[frame.size() - 2] == 0xFF && frame[frame.size() - 1] == 0xD9)
          impl_->publish(std::move(frame));
      } else {
#ifdef _WIN32
        if (impl_->have_ffmpeg && impl_->ff_stdin) {
          DWORD written = 0;
          if (!WriteFile(impl_->ff_stdin, frame.data(),
                         static_cast<DWORD>(frame.size()), &written, nullptr)) {
            if (!impl_->quit) impl_->set_error("FFmpeg berhenti saat membaca H.264");
            break;
          }
        }
#else
        if (impl_->ff_stdin >= 0) {
          if (::write(impl_->ff_stdin, frame.data(), frame.size()) < 0) {
            if (!impl_->quit) impl_->set_error("FFmpeg berhenti saat membaca H.264");
            break;
          }
        }
#endif
      }
    }
  });

  return port;
}

void PreviewPipeline::stop() {
  impl_->quit = true;

#ifdef _WIN32
  if (impl_->sock != INVALID_SOCKET) {
    ::closesocket(impl_->sock);
    impl_->sock = INVALID_SOCKET;
  }
  if (impl_->ff_stdin) { CloseHandle(impl_->ff_stdin); impl_->ff_stdin = nullptr; }
#else
  if (impl_->sock >= 0) { ::close(impl_->sock); impl_->sock = -1; }
  if (impl_->ff_stdin >= 0) { ::close(impl_->ff_stdin); impl_->ff_stdin = -1; }
#endif

  if (impl_->receiver_thread.joinable()) impl_->receiver_thread.join();

#ifdef _WIN32
  if (impl_->have_ffmpeg) {
    // Closing stdin lets FFmpeg drain and exit; kill only if it will not.
    if (WaitForSingleObject(impl_->ffmpeg.hProcess, 2000) != WAIT_OBJECT_0)
      TerminateProcess(impl_->ffmpeg.hProcess, 0);
    CloseHandle(impl_->ffmpeg.hProcess);
    CloseHandle(impl_->ffmpeg.hThread);
    ZeroMemory(&impl_->ffmpeg, sizeof(impl_->ffmpeg));
    impl_->have_ffmpeg = false;
  }
  if (impl_->ff_stdout) { CloseHandle(impl_->ff_stdout); impl_->ff_stdout = nullptr; }
#else
  if (impl_->ffmpeg > 0) {
    int status = 0;
    ::waitpid(impl_->ffmpeg, &status, 0);
    impl_->ffmpeg = -1;
  }
  if (impl_->ff_stdout >= 0) { ::close(impl_->ff_stdout); impl_->ff_stdout = -1; }
#endif

  if (impl_->jpeg_thread.joinable()) impl_->jpeg_thread.join();

  impl_->quit = false;
  std::lock_guard<std::mutex> g(impl_->lock);
  if (impl_->state != PreviewState::Error && impl_->state != PreviewState::Idle)
    impl_->state = PreviewState::Stopped;
}

}  // namespace ucv

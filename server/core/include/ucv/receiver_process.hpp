// Launches and stops ucv-receiver.exe.
//
// The stop path is the subtle part. On Windows, TerminateProcess delivers no
// signal at all, so the receiver dies before writing its summary line — and a
// run with no summary is permanently stuck at clock_status "RUNNING" and can
// never be counted as valid. The Python dashboard hit exactly this and was
// fixed the same way: CTRL_BREAK to a process group, hard kill only as a
// fallback. Anything that starts the receiver has to get this right.

#ifndef UCV_RECEIVER_PROCESS_HPP
#define UCV_RECEIVER_PROCESS_HPP

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ucv {

struct RunRequest {
  std::string phone_ip = "192.168.137.139";
  std::string protocol = "raw_udp";
  std::string run_id;          // generated when empty
  std::string mode;            // "1280x720@30"
  std::string condition = "clean";
  int duration_s = 60;
  int warmup_s = 5;
  int video_port = 0;          // 0 = protocol default
  bool manual_phone = false;   // true = do not remote-START the phone
};

enum class SessionState { Idle, Starting, Running, Complete, Failed };

const char* to_string(SessionState state);

class ReceiverProcess {
 public:
  ReceiverProcess();
  ~ReceiverProcess();

  ReceiverProcess(const ReceiverProcess&) = delete;
  ReceiverProcess& operator=(const ReceiverProcess&) = delete;

  // Absolute path to ucv-receiver.exe. Set once at startup.
  void set_executable(std::filesystem::path exe) { exe_ = std::move(exe); }
  void set_results_dir(std::filesystem::path dir) { results_ = std::move(dir); }

  // Fails (returns false, reason in last_error) rather than queueing when a
  // session is already running: two receivers on one port would both fail in
  // confusing ways.
  bool start(const RunRequest& request, std::string* error);

  // Graceful stop with a bounded wait, then kill. Safe to call when idle.
  void stop();

  SessionState state() const;
  std::string run_id() const;
  std::string last_error() const;

  // Console output collected so far, newest last. Bounded so a long run cannot
  // grow without limit.
  std::vector<std::string> output() const;

  // Called on a worker thread whenever output arrives or the state changes.
  void set_on_update(std::function<void()> callback);

  // Reaps a finished child and updates state. Call periodically from the UI.
  void poll();

 private:
  struct Impl;
  Impl* impl_;
  std::filesystem::path exe_;
  std::filesystem::path results_;
};

// Finds the repository root by walking up from `start` looking for the marker
// directories, so the app works whether launched from the build tree or a
// shortcut.
std::filesystem::path find_repo_root(const std::filesystem::path& start);

}  // namespace ucv

#endif  // UCV_RECEIVER_PROCESS_HPP

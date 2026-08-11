#include "ucv/receiver_process.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ucv {

const char* to_string(SessionState state) {
  switch (state) {
    case SessionState::Idle:     return "idle";
    case SessionState::Starting: return "starting";
    case SessionState::Running:  return "running";
    case SessionState::Complete: return "complete";
    case SessionState::Failed:   return "failed";
  }
  return "idle";
}

struct ReceiverProcess::Impl {
  mutable std::mutex lock;
  SessionState state = SessionState::Idle;
  std::string run_id;
  std::string last_error;
  std::deque<std::string> output;   // bounded
  std::function<void()> on_update;
  std::thread pump;
  std::atomic<bool> pump_quit{false};

#ifdef _WIN32
  PROCESS_INFORMATION pi{};
  HANDLE stdout_read = nullptr;
  bool have_process = false;
#else
  pid_t pid = -1;
#endif

  void push_line(std::string line) {
    std::function<void()> cb;
    {
      std::lock_guard<std::mutex> g(lock);
      output.push_back(std::move(line));
      // 2000 lines is far more than any run produces, and keeps a runaway
      // child from growing this without bound.
      while (output.size() > 2000) output.pop_front();
      cb = on_update;
    }
    if (cb) cb();
  }

  void set_state(SessionState s) {
    std::function<void()> cb;
    {
      std::lock_guard<std::mutex> g(lock);
      state = s;
      cb = on_update;
    }
    if (cb) cb();
  }
};

ReceiverProcess::ReceiverProcess() : impl_(new Impl) {}

ReceiverProcess::~ReceiverProcess() {
  stop();
  impl_->pump_quit = true;
  if (impl_->pump.joinable()) impl_->pump.join();
  delete impl_;
}

void ReceiverProcess::set_on_update(std::function<void()> callback) {
  std::lock_guard<std::mutex> g(impl_->lock);
  impl_->on_update = std::move(callback);
}

SessionState ReceiverProcess::state() const {
  std::lock_guard<std::mutex> g(impl_->lock);
  return impl_->state;
}

std::string ReceiverProcess::run_id() const {
  std::lock_guard<std::mutex> g(impl_->lock);
  return impl_->run_id;
}

std::string ReceiverProcess::last_error() const {
  std::lock_guard<std::mutex> g(impl_->lock);
  return impl_->last_error;
}

std::vector<std::string> ReceiverProcess::output() const {
  std::lock_guard<std::mutex> g(impl_->lock);
  return {impl_->output.begin(), impl_->output.end()};
}

namespace {

std::string generate_run_id() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
  return std::to_string(secs) + "-run";
}

bool safe_text(const std::string& s) {
  if (s.empty() || s.size() > 96) return false;
  for (char c : s) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                    c == '@' || c == '-';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

bool ReceiverProcess::start(const RunRequest& request, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->last_error = msg;
    return false;
  };

  if (state() == SessionState::Running || state() == SessionState::Starting)
    return fail("a receiver session is already running");
  if (exe_.empty() || !std::filesystem::is_regular_file(exe_))
    return fail("receiver binary missing; run experiment/build-receiver.ps1");

  std::string run_id = request.run_id.empty() ? generate_run_id() : request.run_id;
  if (!safe_text(run_id)) return fail("run_id contains unsupported characters");
  if (!safe_text(request.protocol)) return fail("invalid protocol");
  if (!safe_text(request.condition)) return fail("invalid condition");

  // A duplicate run_id would append to an existing log and mix two runs into
  // one file, so a suffix is added rather than silently overwriting.
  {
    std::string candidate = run_id;
    int suffix = 2;
    while (std::filesystem::exists(results_ / ("receiver-" + candidate + ".ndjson"))) {
      candidate = run_id + "-" + std::to_string(suffix++);
    }
    run_id = candidate;
  }

  std::vector<std::string> args;
  args.push_back(exe_.string());
  args.push_back("--phone");     args.push_back(request.phone_ip);
  args.push_back("--protocol");  args.push_back(request.protocol);
  args.push_back("--run-id");    args.push_back(run_id);
  args.push_back("--duration");  args.push_back(std::to_string(request.duration_s));
  args.push_back("--warmup");    args.push_back(std::to_string(request.warmup_s));
  args.push_back("--condition"); args.push_back(request.condition);
  args.push_back("--out");       args.push_back(results_.string());
  if (!request.mode.empty()) { args.push_back("--mode"); args.push_back(request.mode); }
  if (request.video_port > 0) {
    args.push_back("--video-port");
    args.push_back(std::to_string(request.video_port));
  }
  if (request.preview_port > 0) {
    args.push_back("--preview-port");
    args.push_back(std::to_string(request.preview_port));
  }
  if (request.manual_phone) args.push_back("--manual-phone");

  {
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->output.clear();
    impl_->run_id = run_id;
    impl_->last_error.clear();
  }
  impl_->set_state(SessionState::Starting);

#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&impl_->stdout_read, &write_end, &sa, 0))
    return fail("cannot create output pipe");
  SetHandleInformation(impl_->stdout_read, HANDLE_FLAG_INHERIT, 0);

  std::string cmdline;
  for (const std::string& a : args) {
    if (!cmdline.empty()) cmdline += ' ';
    cmdline += '"';
    cmdline += a;
    cmdline += '"';
  }

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.hStdOutput = write_end;
  si.hStdError = write_end;
  si.wShowWindow = SW_HIDE;

  // CREATE_NEW_PROCESS_GROUP so the child can be sent CTRL_BREAK later, which
  // it turns into a clean shutdown that writes the summary line.
  //
  // CREATE_NO_WINDOW must NOT be added: a process with no console receives no
  // console control events at all, the stop request is silently dropped, and
  // the receiver has to be hard killed — producing a log with no summary that
  // can never be counted as a valid run. The window is hidden through
  // STARTUPINFO instead, which keeps a console attached for the signal.
  ZeroMemory(&impl_->pi, sizeof(impl_->pi));
  const BOOL ok = CreateProcessA(
      nullptr, cmdline.data(), nullptr, nullptr, TRUE,
      CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &impl_->pi);
  CloseHandle(write_end);
  if (!ok) {
    CloseHandle(impl_->stdout_read);
    impl_->stdout_read = nullptr;
    impl_->set_state(SessionState::Failed);
    return fail("CreateProcess failed");
  }
  impl_->have_process = true;
#else
  int fds[2];
  if (pipe(fds) != 0) return fail("cannot create output pipe");
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return fail("fork failed");
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    setpgid(0, 0);   // own group, so a signal reaches only the child
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  close(fds[1]);
  impl_->pid = pid;
  impl_->stdout_read = fds[0];
#endif

  impl_->pump_quit = false;
  if (impl_->pump.joinable()) impl_->pump.join();
  impl_->pump = std::thread([this] {
    std::string partial;
    char buffer[4096];
    for (;;) {
#ifdef _WIN32
      DWORD n = 0;
      if (!ReadFile(impl_->stdout_read, buffer, sizeof(buffer), &n, nullptr) || n == 0)
        break;
#else
      const ssize_t n = ::read(impl_->stdout_read, buffer, sizeof(buffer));
      if (n <= 0) break;
#endif
      partial.append(buffer, static_cast<std::size_t>(n));
      std::size_t nl;
      while ((nl = partial.find('\n')) != std::string::npos) {
        std::string line = partial.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        impl_->push_line(std::move(line));
        partial.erase(0, nl + 1);
      }
      if (impl_->state == SessionState::Starting)
        impl_->set_state(SessionState::Running);
    }
    if (!partial.empty()) impl_->push_line(partial);
  });

  impl_->set_state(SessionState::Running);
  return true;
}

void ReceiverProcess::stop() {
#ifdef _WIN32
  if (!impl_->have_process) return;
  // Graceful first: the receiver's console handler turns CTRL_BREAK into a
  // clean exit through WriteSummary(). Only escalate if it does not comply.
  bool exited = false;
  if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, impl_->pi.dwProcessId)) {
    exited = WaitForSingleObject(impl_->pi.hProcess, 10000) == WAIT_OBJECT_0;
  }
  if (!exited) {
    TerminateProcess(impl_->pi.hProcess, 1);
    WaitForSingleObject(impl_->pi.hProcess, 2000);
  }
  CloseHandle(impl_->pi.hProcess);
  CloseHandle(impl_->pi.hThread);
  ZeroMemory(&impl_->pi, sizeof(impl_->pi));
  impl_->have_process = false;
  if (impl_->stdout_read) {
    CloseHandle(impl_->stdout_read);
    impl_->stdout_read = nullptr;
  }
#else
  if (impl_->pid <= 0) return;
  ::kill(-impl_->pid, SIGINT);      // SIGINT is what the receiver handles
  for (int i = 0; i < 100; ++i) {
    int status = 0;
    if (::waitpid(impl_->pid, &status, WNOHANG) == impl_->pid) {
      impl_->pid = -1;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (impl_->pid > 0) {
    ::kill(-impl_->pid, SIGKILL);
    int status = 0;
    ::waitpid(impl_->pid, &status, 0);
    impl_->pid = -1;
  }
  if (impl_->stdout_read >= 0) {
    ::close(impl_->stdout_read);
    impl_->stdout_read = -1;
  }
#endif
  impl_->pump_quit = true;
  if (impl_->pump.joinable()) impl_->pump.join();
  if (impl_->state == SessionState::Running ||
      impl_->state == SessionState::Starting)
    impl_->set_state(SessionState::Complete);
}

void ReceiverProcess::poll() {
#ifdef _WIN32
  if (!impl_->have_process) return;
  DWORD code = 0;
  if (GetExitCodeProcess(impl_->pi.hProcess, &code) && code != STILL_ACTIVE) {
    CloseHandle(impl_->pi.hProcess);
    CloseHandle(impl_->pi.hThread);
    ZeroMemory(&impl_->pi, sizeof(impl_->pi));
    impl_->have_process = false;
    if (impl_->stdout_read) {
      CloseHandle(impl_->stdout_read);
      impl_->stdout_read = nullptr;
    }
    if (impl_->pump.joinable()) impl_->pump.join();
    impl_->set_state(code == 0 ? SessionState::Complete : SessionState::Failed);
  }
#else
  if (impl_->pid <= 0) return;
  int status = 0;
  if (::waitpid(impl_->pid, &status, WNOHANG) == impl_->pid) {
    impl_->pid = -1;
    if (impl_->pump.joinable()) impl_->pump.join();
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    impl_->set_state(ok ? SessionState::Complete : SessionState::Failed);
  }
#endif
}

std::filesystem::path find_repo_root(const std::filesystem::path& start) {
  std::filesystem::path dir = std::filesystem::absolute(start);
  for (int i = 0; i < 8 && !dir.empty(); ++i) {
    if (std::filesystem::exists(dir / "experiment" / "receiver") &&
        std::filesystem::exists(dir / "app")) {
      return dir;
    }
    const std::filesystem::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return {};
}

}  // namespace ucv

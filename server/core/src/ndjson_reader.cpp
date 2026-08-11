#include "ucv/ndjson_reader.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ucv {
namespace {

// ---------------------------------------------------------------------
// Minimal flat-JSON scanning
// ---------------------------------------------------------------------
// Only what ucv_log.cpp emits: one flat object per line, string and numeric
// values, no nesting. Anything else is treated as absent rather than guessed at.

std::string_view find_value(std::string_view line, std::string_view key) {
  // Match "key": including the quotes, so a key that is a prefix of another
  // (e.g. "packets_lost" vs "packets_lost_total") cannot match by accident.
  std::string needle;
  needle.reserve(key.size() + 3);
  needle += '"';
  needle += key;
  needle += "\":";
  const std::size_t pos = line.find(needle);
  if (pos == std::string_view::npos) return {};
  std::size_t v = pos + needle.size();
  while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
  if (v >= line.size()) return {};
  if (line[v] == '"') {
    const std::size_t end = line.find('"', v + 1);
    if (end == std::string_view::npos) return {};
    return line.substr(v + 1, end - v - 1);
  }
  std::size_t end = v;
  while (end < line.size() && line[end] != ',' && line[end] != '}') ++end;
  while (end > v && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
  return line.substr(v, end - v);
}

template <typename T>
T parse_number(std::string_view text, T fallback) {
  if (text.empty()) return fallback;
  T out{};
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  if constexpr (std::is_floating_point_v<T>) {
    // from_chars for double is not available everywhere in libstdc++ 8/9; a
    // stringstream is slower but only runs on control records (one per run).
    std::string tmp(text);
    try {
      return static_cast<T>(std::stod(tmp));
    } catch (...) {
      return fallback;
    }
  } else {
    const auto res = std::from_chars(begin, end, out);
    return res.ec == std::errc{} ? out : fallback;
  }
}

bool parse_bool(std::string_view text, bool fallback) {
  if (text == "true") return true;
  if (text == "false") return false;
  return fallback;
}

}  // namespace

RunLog read_run_log(const std::filesystem::path& path,
                    const std::filesystem::path& results_root) {
  RunLog log;
  {
    std::error_code ec;
    const auto rel = std::filesystem::relative(path, results_root, ec);
    log.file = ec ? path.filename().string() : rel.generic_string();
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    log.read_error = "cannot open " + path.string();
    return log;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 2 || line.front() != '{') {
      if (!line.empty()) ++log.parse_errors;
      continue;
    }
    // A run in progress can end mid-line; treat that as "not yet", not corrupt.
    if (line.back() != '}') {
      ++log.parse_errors;
      continue;
    }
    const std::string_view sv{line};
    const std::string_view type = find_value(sv, "type");

    if (type == "frame") {
      FrameRecord f;
      f.seq = parse_number<std::uint32_t>(find_value(sv, "seq"), 0);
      f.cap_ns = parse_number<std::uint64_t>(find_value(sv, "cap_ns"), 0);
      f.enc_ns = parse_number<std::uint64_t>(find_value(sv, "enc_ns"), 0);
      f.snd_ns = parse_number<std::uint64_t>(find_value(sv, "snd_ns"), 0);
      f.rcv_ns = parse_number<std::uint64_t>(find_value(sv, "rcv_ns"), 0);
      f.bytes = parse_number<std::uint32_t>(find_value(sv, "bytes"), 0);
      f.keyframe = parse_bool(find_value(sv, "key"), false);
      f.packets_received =
          parse_number<std::uint64_t>(find_value(sv, "packets_received"), 0);
      f.packets_lost =
          parse_number<std::uint64_t>(find_value(sv, "packets_lost"), 0);
      f.packets_recovered =
          parse_number<std::uint64_t>(find_value(sv, "packets_recovered"), 0);
      log.frames.push_back(f);
    } else if (type == "meta") {
      MetaRecord& m = log.meta;
      m.run_id = std::string(find_value(sv, "run_id"));
      m.protocol = std::string(find_value(sv, "protocol"));
      m.mode = std::string(find_value(sv, "mode"));
      m.condition = std::string(find_value(sv, "condition"));
      m.clock_offset_ns =
          parse_number<std::int64_t>(find_value(sv, "clock_offset_ns"), 0);
      m.clock_best_rtt_ns =
          parse_number<std::int64_t>(find_value(sv, "clock_best_rtt_ns"), 0);
    } else if (type == "control") {
      ControlRecord c;
      c.sent = parse_number<std::uint64_t>(find_value(sv, "sent"), 0);
      c.acked = parse_number<std::uint64_t>(find_value(sv, "acked"), 0);
      c.p50_ms = parse_number<double>(find_value(sv, "p50_ms"), 0.0);
      c.p95_ms = parse_number<double>(find_value(sv, "p95_ms"), 0.0);
      c.p99_ms = parse_number<double>(find_value(sv, "p99_ms"), 0.0);
      c.max_ms = parse_number<double>(find_value(sv, "max_ms"), 0.0);
      log.control = c;
    } else if (type == "summary") {
      SummaryRecord s;
      s.frames_received =
          parse_number<std::uint64_t>(find_value(sv, "frames_received"), 0);
      s.gap_frames = parse_number<std::uint64_t>(find_value(sv, "gap_frames"), 0);
      s.reorder_events =
          parse_number<std::uint64_t>(find_value(sv, "reorder_events"), 0);
      s.duplicates = parse_number<std::uint64_t>(find_value(sv, "duplicates"), 0);
      s.reassembly_failures =
          parse_number<std::uint64_t>(find_value(sv, "reassembly_failures"), 0);
      s.bytes_payload =
          parse_number<std::uint64_t>(find_value(sv, "bytes_payload"), 0);
      s.bytes_wire = parse_number<std::uint64_t>(find_value(sv, "bytes_wire"), 0);
      s.measurement_duration_ns = parse_number<std::uint64_t>(
          find_value(sv, "measurement_duration_ns"), 0);
      s.packets_received =
          parse_number<std::uint64_t>(find_value(sv, "packets_received"), 0);
      s.packets_lost =
          parse_number<std::uint64_t>(find_value(sv, "packets_lost"), 0);
      s.packets_recovered =
          parse_number<std::uint64_t>(find_value(sv, "packets_recovered"), 0);
      s.packet_reorder_events =
          parse_number<std::uint64_t>(find_value(sv, "packet_reorder_events"), 0);
      s.packet_duplicates =
          parse_number<std::uint64_t>(find_value(sv, "packet_duplicates"), 0);
      s.clock_offset_after_ns =
          parse_number<std::int64_t>(find_value(sv, "clock_offset_after_ns"), 0);
      s.clock_drift_ns =
          parse_number<std::int64_t>(find_value(sv, "clock_drift_ns"), 0);
      s.clock_status = std::string(find_value(sv, "clock_status"));
      log.summary = s;
    }
  }
  return log;
}

std::vector<std::filesystem::path> list_run_files(
    const std::filesystem::path& results_root) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  if (!std::filesystem::exists(results_root, ec)) return files;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(results_root, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("receiver-", 0) == 0 &&
        entry.path().extension() == ".ndjson") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              std::error_code e1, e2;
              const auto ta = std::filesystem::last_write_time(a, e1);
              const auto tb = std::filesystem::last_write_time(b, e2);
              return ta > tb;   // newest first
            });
  return files;
}

// ---------------------------------------------------------------------
// LabelStore
// ---------------------------------------------------------------------
// Same .dashboard-labels.json the Python dashboard uses, so a comparison group
// set in one front end shows up in the other. Only the "group" field is read
// and written here; "mode"/"condition" overrides belong to older logs and are
// preserved untouched by rewriting only the keys this app owns.

LabelStore::LabelStore(std::filesystem::path results_root)
    : path_(std::move(results_root) / ".dashboard-labels.json") {
  reload();
}

void LabelStore::reload() {
  groups_.clear();
  std::ifstream in(path_, std::ios::binary);
  if (!in) return;
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();

  // The file is a flat object of objects: {"file.ndjson": {"group": "..."}}.
  std::size_t pos = 0;
  while (true) {
    const std::size_t key_start = text.find('"', pos);
    if (key_start == std::string::npos) break;
    const std::size_t key_end = text.find('"', key_start + 1);
    if (key_end == std::string::npos) break;
    const std::string key = text.substr(key_start + 1, key_end - key_start - 1);
    const std::size_t brace = text.find('{', key_end);
    if (brace == std::string::npos) break;
    const std::size_t close = text.find('}', brace);
    if (close == std::string::npos) break;
    const std::string_view body{text.data() + brace, close - brace + 1};
    const std::string_view group = find_value(body, "group");
    if (!group.empty()) groups_.emplace_back(key, std::string(group));
    pos = close + 1;
  }
}

std::string LabelStore::group_for(const std::string& relative_file) const {
  for (const auto& [file, group] : groups_) {
    if (file == relative_file) return group;
  }
  return {};
}

bool LabelStore::set_group(const std::string& relative_file,
                           const std::string& group) {
  // Read-modify-write of the whole file, preserving entries this app does not
  // own. The Python dashboard does the same, and both write via a temp file
  // and rename so a crash cannot leave a half-written label store.
  reload();
  bool replaced = false;
  for (auto& [file, existing] : groups_) {
    if (file == relative_file) {
      existing = group;
      replaced = true;
      break;
    }
  }
  if (!replaced && !group.empty()) groups_.emplace_back(relative_file, group);

  std::string out = "{\n";
  bool first = true;
  for (const auto& [file, g] : groups_) {
    if (g.empty()) continue;   // clearing a group removes the entry
    if (!first) out += ",\n";
    first = false;
    out += "  \"";
    out += file;
    out += "\": {\n    \"group\": \"";
    for (char c : g) {
      if (c == '"' || c == '\\') out += '\\';
      out += c;
    }
    out += "\"\n  }";
  }
  out += "\n}\n";

  const std::filesystem::path tmp = path_.string() + ".tmp";
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) return false;
    os << out;
    if (!os) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path_, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

}  // namespace ucv

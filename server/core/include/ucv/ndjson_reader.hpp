// Reads the receiver's NDJSON logs.
//
// Deliberately hand-rolled rather than pulling in a JSON library: the receiver
// writes a small, fixed set of flat objects with no nesting, no arrays and no
// unicode escapes beyond what JsonEscape produces. A scanner for that is short
// enough to audit, and it keeps monitor-core dependency-free so it can be built
// and tested on a machine with no Qt and no package manager.
//
// A malformed line is counted and skipped, never fatal: the last line of a run
// in progress is routinely half-written, and refusing to parse the file because
// of it would blank the live charts exactly when they matter.

#ifndef UCV_NDJSON_READER_HPP
#define UCV_NDJSON_READER_HPP

#include "ucv/run_data.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ucv {

// Reads one run log. Never throws; failures land in RunLog::read_error.
RunLog read_run_log(const std::filesystem::path& path,
                    const std::filesystem::path& results_root);

// Lists receiver-*.ndjson under `results_root`, newest first by mtime.
std::vector<std::filesystem::path> list_run_files(
    const std::filesystem::path& results_root);

// Operator-assigned comparison groups, shared with the Python dashboard via
// the same .dashboard-labels.json file so a label set in one is visible in the
// other.
struct LabelStore {
  explicit LabelStore(std::filesystem::path results_root);

  // Empty string when the run has no group.
  std::string group_for(const std::string& relative_file) const;
  bool set_group(const std::string& relative_file, const std::string& group);
  void reload();

 private:
  std::filesystem::path path_;
  // relative file -> group
  std::vector<std::pair<std::string, std::string>> groups_;
};

}  // namespace ucv

#endif  // UCV_NDJSON_READER_HPP

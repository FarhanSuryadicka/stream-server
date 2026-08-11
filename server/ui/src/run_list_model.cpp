#include "run_list_model.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QSet>

#include <algorithm>
#include <map>

namespace {

QVariantMap to_map(const ucv::RunMetrics& m) {
  QVariantMap v;
  v["run_id"] = QString::fromStdString(m.run_id);
  v["file"] = QString::fromStdString(m.file);
  v["protocol"] = QString::fromStdString(m.protocol);
  v["mode"] = QString::fromStdString(m.mode);
  v["condition"] = QString::fromStdString(m.condition);
  v["group"] = QString::fromStdString(m.group);
  v["clock_status"] = QString::fromStdString(m.clock_status);
  v["valid"] = m.valid;
  v["frames"] = static_cast<qulonglong>(m.frames);
  v["fps"] = m.fps;
  v["p50"] = m.transport_ms.p50;
  v["p95"] = m.transport_ms.p95;
  v["p99"] = m.transport_ms.p99;
  v["max"] = m.transport_ms.max;
  v["glass_p50"] = m.glass_ms.p50;
  v["encode_p50"] = m.encode_ms.p50;
  v["jitter"] = m.jitter_ms;
  v["loss"] = m.loss_percent;
  v["loss_basis"] = QString::fromLatin1(ucv::to_string(m.loss_basis));
  v["loss_label"] = QString::fromLatin1(ucv::loss_label(m.loss_basis));
  v["goodput"] = m.goodput_mbps;
  // Absent rather than 0 when the protocol exposes no wire total: QML shows
  // "n/a" instead of a number that was never measured.
  if (m.overhead_percent) v["overhead"] = *m.overhead_percent;
  if (m.control) {
    v["control_p50"] = m.control->p50_ms;
    v["control_p95"] = m.control->p95_ms;
    v["control_p99"] = m.control->p99_ms;
    v["control_sent"] = static_cast<qulonglong>(m.control->sent);
    v["control_acked"] = static_cast<qulonglong>(m.control->acked);
  }
  return v;
}

}  // namespace

// ---------------------------------------------------------------- RunListModel

RunListModel::RunListModel(QObject* parent) : QAbstractListModel(parent) {}

void RunListModel::setResultsDir(const std::filesystem::path& dir) {
  results_ = dir;
  labels_ = std::make_unique<ucv::LabelStore>(dir);
  refresh();
}

void RunListModel::refresh() {
  if (results_.empty()) return;
  if (labels_) labels_->reload();
  std::vector<ucv::RunMetrics> next;
  for (const auto& file : ucv::list_run_files(results_)) {
    const ucv::RunLog log = ucv::read_run_log(file, results_);
    if (log.read_error) continue;
    ucv::RunMetrics m = ucv::compute_metrics(log, false);
    if (labels_) m.group = labels_->group_for(m.file);
    next.push_back(std::move(m));
  }
  beginResetModel();
  runs_ = std::move(next);
  endResetModel();
}

int RunListModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(runs_.size());
}

QVariant RunListModel::data(const QModelIndex& index, int role) const {
  if (index.row() < 0 || index.row() >= static_cast<int>(runs_.size()))
    return {};
  const ucv::RunMetrics& m = runs_[static_cast<std::size_t>(index.row())];
  switch (role) {
    case RunIdRole:      return QString::fromStdString(m.run_id);
    case FileRole:       return QString::fromStdString(m.file);
    case ProtocolRole:   return QString::fromStdString(m.protocol);
    case ModeRole:       return QString::fromStdString(m.mode);
    case GroupRole:      return QString::fromStdString(m.group);
    case ClockStatusRole:return QString::fromStdString(m.clock_status);
    case ValidRole:      return m.valid;
    case FramesRole:     return static_cast<qulonglong>(m.frames);
    case P50Role:        return m.transport_ms.p50;
    case P95Role:        return m.transport_ms.p95;
    case P99Role:        return m.transport_ms.p99;
    case JitterRole:     return m.jitter_ms;
    case LossRole:       return m.loss_percent;
    case LossBasisRole:  return QString::fromLatin1(ucv::to_string(m.loss_basis));
    case GoodputRole:    return m.goodput_mbps;
    default:             return {};
  }
}

QHash<int, QByteArray> RunListModel::roleNames() const {
  return {
      {RunIdRole, "runId"},         {FileRole, "file"},
      {ProtocolRole, "protocol"},   {ModeRole, "mode"},
      {GroupRole, "group"},         {ClockStatusRole, "clockStatus"},
      {ValidRole, "valid"},         {FramesRole, "frames"},
      {P50Role, "p50"},             {P95Role, "p95"},
      {P99Role, "p99"},             {JitterRole, "jitter"},
      {LossRole, "loss"},           {LossBasisRole, "lossBasis"},
      {GoodputRole, "goodput"},
  };
}

// ------------------------------------------------------------ ComparisonModel

ComparisonModel::ComparisonModel(QObject* parent) : QAbstractListModel(parent) {}

void ComparisonModel::rebuild(const std::vector<ucv::RunMetrics>& runs) {
  std::map<QString, Group> by_group;
  for (const ucv::RunMetrics& m : runs) {
    if (m.group.empty()) continue;   // manual opt-in only
    // Protocol is part of the key: averaging RTMP and HLS into one row would
    // produce a figure describing neither.
    const QString key = QString::fromStdString(m.protocol) + " / " +
                        QString::fromStdString(m.group);
    Group& g = by_group[key];
    g.name = key;
    if (m.valid) {
      g.valid_runs++;
      g.frames += m.frames;
      g.p50 += m.transport_ms.p50;
      g.p95 += m.transport_ms.p95;
      g.p99 += m.transport_ms.p99;
      g.jitter += m.jitter_ms;
      g.loss += m.loss_percent;
      g.goodput += m.goodput_mbps;
      const QString basis = QString::fromLatin1(ucv::to_string(m.loss_basis));
      if (g.loss_basis.isEmpty()) g.loss_basis = basis;
      else if (g.loss_basis != basis) g.mixed_basis = true;
      g.run_ids << QString::fromStdString(m.run_id);
    } else if (m.clock_status == "RUNNING") {
      g.pending++;
    }
  }

  std::vector<Group> next;
  for (auto& [key, g] : by_group) {
    if (g.valid_runs > 0) {
      const double n = g.valid_runs;
      g.p50 /= n; g.p95 /= n; g.p99 /= n;
      g.jitter /= n; g.loss /= n; g.goodput /= n;
    }
    if (g.valid_runs || g.pending) next.push_back(g);
  }
  beginResetModel();
  groups_ = std::move(next);
  endResetModel();
}

int ComparisonModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(groups_.size());
}

QVariant ComparisonModel::data(const QModelIndex& index, int role) const {
  if (index.row() < 0 || index.row() >= static_cast<int>(groups_.size()))
    return {};
  const Group& g = groups_[static_cast<std::size_t>(index.row())];
  switch (role) {
    case GroupRole:      return g.name;
    case RunCountRole:   return g.valid_runs;
    case PendingRole:    return g.pending;
    case FramesRole:     return static_cast<qulonglong>(g.frames);
    case P50Role:        return g.p50;
    case P95Role:        return g.p95;
    case P99Role:        return g.p99;
    case JitterRole:     return g.jitter;
    case LossRole:       return g.loss;
    case LossBasisRole:  return g.loss_basis;
    case MixedBasisRole: return g.mixed_basis;
    case GoodputRole:    return g.goodput;
    case RunIdsRole:     return g.run_ids;
    default:             return {};
  }
}

QHash<int, QByteArray> ComparisonModel::roleNames() const {
  return {
      {GroupRole, "group"},         {RunCountRole, "runCount"},
      {PendingRole, "pending"},     {FramesRole, "frames"},
      {P50Role, "p50"},             {P95Role, "p95"},
      {P99Role, "p99"},             {JitterRole, "jitter"},
      {LossRole, "loss"},           {LossBasisRole, "lossBasis"},
      {MixedBasisRole, "mixedBasis"},{GoodputRole, "goodput"},
      {RunIdsRole, "runIds"},
  };
}

// --------------------------------------------------------- MonitorController

MonitorController::MonitorController(QObject* parent) : QObject(parent) {
  repo_root_ = ucv::find_repo_root(
      QCoreApplication::applicationDirPath().toStdString());
  if (repo_root_.empty())
    repo_root_ = ucv::find_repo_root(QDir::currentPath().toStdString());

  if (!repo_root_.empty()) {
    results_ = repo_root_ / "experiment" / "results";
    std::error_code ec;
    std::filesystem::create_directories(results_, ec);
    receiver_.set_results_dir(results_);
    receiver_.set_executable(repo_root_ / "experiment" / "receiver" / "build" /
#ifdef _WIN32
                             "ucv-receiver.exe"
#else
                             "ucv-receiver"
#endif
    );
    runs_.setResultsDir(results_);
    comparison_.rebuild(runs_.runs());
  }

  // One timer drives everything, like the web dashboard's 1 Hz poll: session
  // state, the active run's charts, and periodically the history list.
  connect(&timer_, &QTimer::timeout, this, &MonitorController::tick);
  timer_.start(1000);
  emit pathsChanged();
}

MonitorController::~MonitorController() { receiver_.stop(); }

QString MonitorController::sessionState() const {
  return QString::fromLatin1(ucv::to_string(receiver_.state()));
}

QString MonitorController::consoleText() const {
  const auto lines = receiver_.output();
  QStringList out;
  out.reserve(static_cast<int>(lines.size()));
  for (const std::string& l : lines) out << QString::fromStdString(l);
  return out.isEmpty() ? QStringLiteral("No active session.") : out.join('\n');
}

QString MonitorController::activeRunId() const {
  return QString::fromStdString(receiver_.run_id());
}

bool MonitorController::running() const {
  const auto s = receiver_.state();
  return s == ucv::SessionState::Running || s == ucv::SessionState::Starting;
}

QString MonitorController::resultsDir() const {
  return QString::fromStdString(results_.string());
}

bool MonitorController::receiverPresent() const {
  if (repo_root_.empty()) return false;
  const auto exe = repo_root_ / "experiment" / "receiver" / "build" /
#ifdef _WIN32
                   "ucv-receiver.exe";
#else
                   "ucv-receiver";
#endif
  return std::filesystem::is_regular_file(exe);
}

QStringList MonitorController::protocols() const {
  // Matches the receiver's --protocol list; adding one here without adding it
  // there would produce a run the receiver refuses to start.
  return {"raw_udp", "rtp_udp", "rtsp", "srt", "mjpeg", "hls", "rtmp", "webrtc"};
}

QVariantMap MonitorController::selectedRun() const {
  if (pending_placeholder_) {
    QVariantMap v;
    v["run_id"] = activeRunId();
    v["clock_status"] = QStringLiteral("STARTING");
    v["pending"] = true;
    return v;
  }
  if (!selected_) return {};
  return to_map(*selected_);
}

QVariantList MonitorController::series() const {
  QVariantList out;
  if (!selected_) return out;
  out.reserve(static_cast<int>(selected_->series.size()));
  for (const ucv::SeriesPoint& p : selected_->series) {
    QVariantMap m;
    m["t"] = p.t_seconds;
    m["latency"] = p.latency_ms;
    m["jitter"] = p.jitter_ms;
    m["loss"] = p.loss_percent;
    out.push_back(m);
  }
  return out;
}

void MonitorController::startRun(const QString& phone, const QString& protocol,
                                 const QString& runId, const QString& mode,
                                 int duration, int warmup, bool manualPhone) {
  ucv::RunRequest req;
  req.phone_ip = phone.toStdString();
  req.protocol = protocol.toStdString();
  req.run_id = runId.trimmed().toStdString();
  req.mode = mode.trimmed().toStdString();
  req.duration_s = duration;
  req.warmup_s = warmup;
  req.manual_phone = manualPhone;

  std::string error;
  if (!receiver_.start(req, &error)) {
    emit errorRaised(QString::fromStdString(error));
    return;
  }
  active_file_ = "receiver-" + activeRunId() + ".ndjson";
  selected_file_ = active_file_;
  pending_placeholder_ = true;
  selected_.reset();
  emit sessionChanged();
  emit selectionChanged();
}

void MonitorController::stopRun() {
  receiver_.stop();
  emit sessionChanged();
}

void MonitorController::selectRun(const QString& file) {
  selected_file_ = file;
  pending_placeholder_ = false;
  reselect();
  emit selectionChanged();
}

bool MonitorController::saveGroup(const QString& group) {
  if (results_.empty() || selected_file_.isEmpty()) return false;
  ucv::LabelStore store(results_);
  if (!store.set_group(selected_file_.toStdString(), group.trimmed().toStdString()))
    return false;
  runs_.refresh();
  comparison_.rebuild(runs_.runs());
  reselect();
  emit selectionChanged();
  return true;
}

void MonitorController::refreshRuns() {
  runs_.refresh();
  comparison_.rebuild(runs_.runs());
  emit selectionChanged();
}

void MonitorController::reselect() {
  selected_.reset();
  if (results_.empty() || selected_file_.isEmpty()) return;
  const auto path = results_ / selected_file_.toStdString();
  if (!std::filesystem::is_regular_file(path)) return;
  const ucv::RunLog log = ucv::read_run_log(path, results_);
  if (log.read_error) return;
  ucv::RunMetrics m = ucv::compute_metrics(log, true);
  ucv::LabelStore store(results_);
  m.group = store.group_for(m.file);
  selected_ = std::move(m);
  // Real data has arrived; the "starting" placeholder must not persist, or the
  // panel stays blank for the whole run.
  pending_placeholder_ = false;
}

void MonitorController::tick() {
  const auto before = receiver_.state();
  receiver_.poll();
  const auto after = receiver_.state();

  const bool live = running();
  if (live && !activeRunId().isEmpty()) {
    active_file_ = "receiver-" + activeRunId() + ".ndjson";
    // A run in progress wins over whatever row was last clicked, so the charts
    // always follow the stream that is actually flowing.
    selected_file_ = active_file_;
  } else if (!live) {
    active_file_.clear();
  }

  reselect();
  emit selectionChanged();
  emit sessionChanged();

  if (before != after) {
    runs_.refresh();
    comparison_.rebuild(runs_.runs());
  } else {
    static int counter = 0;
    if (++counter % 5 == 0) {   // history list every 5 s, like the web version
      runs_.refresh();
      comparison_.rebuild(runs_.runs());
    }
  }
}

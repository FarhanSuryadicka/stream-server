// Qt models and controller wrapping monitor-core.
//
// The Qt layer holds no measurement logic — it reads what ucv-core computes and
// presents it. That boundary is the whole point of the core/UI split: a bug in
// a chart cannot change a number, and the numbers stay testable without a
// display (FUTURE-SERVER-ARCHITECTURE.md section 7).

#ifndef UCV_RUN_LIST_MODEL_HPP
#define UCV_RUN_LIST_MODEL_HPP

#include "ucv/metrics.hpp"
#include "ucv/ndjson_reader.hpp"
#include "ucv/receiver_process.hpp"

#include <QAbstractListModel>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <filesystem>
#include <memory>
#include <vector>

// One row in the run history table.
class RunListModel : public QAbstractListModel {
  Q_OBJECT
 public:
  enum Roles {
    RunIdRole = Qt::UserRole + 1,
    FileRole,
    ProtocolRole,
    ModeRole,
    GroupRole,
    ClockStatusRole,
    ValidRole,
    FramesRole,
    P50Role,
    P95Role,
    P99Role,
    JitterRole,
    LossRole,
    LossBasisRole,
    GoodputRole,
  };

  explicit RunListModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  void setResultsDir(const std::filesystem::path& dir);
  void refresh();

  const std::vector<ucv::RunMetrics>& runs() const { return runs_; }

 private:
  std::filesystem::path results_;
  std::vector<ucv::RunMetrics> runs_;
  std::unique_ptr<ucv::LabelStore> labels_;
};

// Aggregated comparison groups. Mirrors the web dashboard: a run appears here
// only once the operator has assigned it a group, and averages count valid runs
// only. Which runs belong in a comparison is a judgement call — bring-up runs
// and retries all finish cleanly but must not be averaged in — so it stays a
// deliberate step rather than something that happens automatically.
class ComparisonModel : public QAbstractListModel {
  Q_OBJECT
 public:
  enum Roles {
    GroupRole = Qt::UserRole + 1,
    RunCountRole,
    PendingRole,
    FramesRole,
    P50Role,
    P95Role,
    P99Role,
    JitterRole,
    LossRole,
    LossBasisRole,
    MixedBasisRole,
    GoodputRole,
    RunIdsRole,
  };

  explicit ComparisonModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  void rebuild(const std::vector<ucv::RunMetrics>& runs);

 private:
  struct Group {
    QString name;
    int valid_runs = 0;
    int pending = 0;
    quint64 frames = 0;
    double p50 = 0, p95 = 0, p99 = 0, jitter = 0, loss = 0, goodput = 0;
    QString loss_basis;
    bool mixed_basis = false;
    QStringList run_ids;
  };
  std::vector<Group> groups_;
};

// Owns the session: starts and stops the receiver, polls the active log, and
// exposes everything QML binds to.
class MonitorController : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString sessionState READ sessionState NOTIFY sessionChanged)
  Q_PROPERTY(QString consoleText READ consoleText NOTIFY sessionChanged)
  Q_PROPERTY(QString activeRunId READ activeRunId NOTIFY sessionChanged)
  Q_PROPERTY(bool running READ running NOTIFY sessionChanged)
  Q_PROPERTY(QString resultsDir READ resultsDir NOTIFY pathsChanged)
  Q_PROPERTY(bool receiverPresent READ receiverPresent NOTIFY pathsChanged)
  Q_PROPERTY(QString selectedFile READ selectedFile NOTIFY selectionChanged)
  Q_PROPERTY(QVariantMap selectedRun READ selectedRun NOTIFY selectionChanged)
  Q_PROPERTY(QVariantList series READ series NOTIFY selectionChanged)
  Q_PROPERTY(QStringList protocols READ protocols CONSTANT)

 public:
  explicit MonitorController(QObject* parent = nullptr);
  ~MonitorController() override;

  QString sessionState() const;
  QString consoleText() const;
  QString activeRunId() const;
  bool running() const;
  QString resultsDir() const;
  bool receiverPresent() const;
  QString selectedFile() const { return selected_file_; }
  QVariantMap selectedRun() const;
  QVariantList series() const;
  QStringList protocols() const;

  RunListModel* runs() { return &runs_; }
  ComparisonModel* comparison() { return &comparison_; }

  Q_INVOKABLE void startRun(const QString& phone, const QString& protocol,
                            const QString& runId, const QString& mode,
                            int duration, int warmup, bool manualPhone);
  Q_INVOKABLE void stopRun();
  Q_INVOKABLE void selectRun(const QString& file);
  Q_INVOKABLE bool saveGroup(const QString& group);
  Q_INVOKABLE void refreshRuns();

 signals:
  void sessionChanged();
  void selectionChanged();
  void pathsChanged();
  void errorRaised(const QString& message);

 private:
  void tick();
  void reselect();

  ucv::ReceiverProcess receiver_;
  RunListModel runs_;
  ComparisonModel comparison_;
  QTimer timer_;
  std::filesystem::path repo_root_;
  std::filesystem::path results_;

  QString selected_file_;
  // The run the operator clicked. During a session the ACTIVE run wins, so the
  // charts always show the stream that is actually flowing — the web dashboard
  // needed the same rule after selection-by-click left the graphs frozen.
  QString active_file_;
  std::optional<ucv::RunMetrics> selected_;
  bool pending_placeholder_ = false;
};

#endif  // UCV_RUN_LIST_MODEL_HPP

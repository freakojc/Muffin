#pragma once

#include <QObject>
#include <functional>
#include <memory>

#include "Export.h"
#include "document/PieceTable.h"

namespace muffin {
class DocumentSession;

// One serial I/O queue per window. Requests own independent piece tables;
// workers never read a live session. Cleanup is a barrier after an in-flight
// write and before any newer request for that key. Destruction drains barriers.
class MUFFIN_CORE_EXPORT DraftSnapshotWriter final : public QObject {
  Q_OBJECT
public:
  using Write = std::function<bool(const PieceTable&, const QString&, const QString&)>;
  explicit DraftSnapshotWriter(QString directory = {}, Write write = {}, QObject* parent = nullptr);
  ~DraftSnapshotWriter() override;
  bool request(const DocumentSession& session, const QString& key);
  void markClean(const QString& sourcePath, const QString& key);
  void waitForDone(); // shutdown/tests only; never used on the input path
  struct QueueStats {
    qsizetype pending = 0, running = 0, pendingCodeUnits = 0, runningCodeUnits = 0;
  };
  QueueStats queueStats() const; // Small diagnostic snapshot; no text copies.

signals:
  void snapshotFinished(const QString& key, quint64 revision, bool saved);

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace muffin

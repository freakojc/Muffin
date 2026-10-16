#include "app/DraftSnapshotWriter.h"

#include <QHash>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

#include "app/DraftRecovery.h"
#include "diagnostics/ScopedPerfProbe.h"
#include "document/DocumentSession.h"

namespace muffin {
namespace {
Q_LOGGING_CATEGORY(draftQueuePerf, "muffin.perf.detail", QtWarningMsg)
}

struct DraftSnapshotWriter::State {
  struct Request {
    PieceTable text;
    QString path;
    quint64 revision = 0, generation = 0;
  };
  struct Entry {
    std::optional<Request> pending;
    QString path;
    quint64 generation = 0, runningRevision = 0;
    bool queued = false, running = false, clean = false;
    qsizetype runningCodeUnits = 0;
  };
  DraftSnapshotWriter* owner;
  DraftRecovery recovery;
  Write write;
  std::mutex mutex;
  std::condition_variable changed;
  QHash<QString, Entry> entries;
  std::deque<QString> ready;
  bool stopping = false;
  quint64 nextGeneration = 0;
  std::thread worker;

  State(DraftSnapshotWriter* object, QString directory, Write function)
      : owner(object), recovery(std::move(directory)), write(std::move(function)) {}

  void enqueue(const QString& key, Entry& entry) {
    if (!entry.queued && !entry.running) {
      entry.queued = true;
      ready.push_back(key);
    }
    if (!worker.joinable()) worker = std::thread([this] { run(); });
    changed.notify_all();
  }

  void run() {
    for (;;) {
      std::unique_lock lock(mutex);
      changed.wait(lock, [this] { return stopping || !ready.empty(); });
      if (ready.empty()) return;
      const QString key = std::move(ready.front());
      ready.pop_front();
      auto& entry = entries[key];
      entry.queued = false;
      entry.running = true;
      const bool clean = std::exchange(entry.clean, false);
      const QString path = entry.path;
      std::optional<Request> request;
      if (!clean) request = std::exchange(entry.pending, std::nullopt);
      entry.runningRevision = request ? request->revision : 0;
      entry.runningCodeUnits = request ? request->text.size() : 0;
      const auto generation = entry.generation;
      lock.unlock();
      bool saved = false;
      if (clean) {
        recovery.markClean(path, key);
      } else if (request) {
        diag::ScopedPerfProbe perf("draft.workerSnapshot", draftQueuePerf());
        if (write)
          saved = write(request->text, request->path, key);
        else
          saved = recovery.snapshot(request->text.toString(), request->path, key);
      }
      lock.lock();
      auto& finished = entries[key];
      finished.running = false;
      finished.runningRevision = 0;
      finished.runningCodeUnits = 0;
      if (request && generation == finished.generation && !finished.clean) {
        QMetaObject::invokeMethod(
            owner,
            [this, key, revision = request->revision, generation, saved] {
              diag::ScopedPerfProbe completionPerf("draft.deliverCompletion", draftQueuePerf());
              // A save/close can invalidate the completion after it was posted.
              std::unique_lock completionLock(mutex);
              const auto it = entries.constFind(key);
              if (it == entries.cend() || it->generation != generation || it->clean) return;
              completionLock.unlock();
              emit owner->snapshotFinished(key, revision, saved);
            },
            Qt::QueuedConnection);
      }
      if (finished.clean || finished.pending)
        enqueue(key, finished);
      else if (clean)
        entries.remove(key);
      request.reset();
      changed.notify_all();
    }
  }
};

DraftSnapshotWriter::DraftSnapshotWriter(QString directory, Write write, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(directory), std::move(write))) {}

DraftSnapshotWriter::~DraftSnapshotWriter() {
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    state_->changed.notify_all();
  }
  if (state_->worker.joinable()) state_->worker.join();
}

bool DraftSnapshotWriter::request(const DocumentSession& session, const QString& key) {
  diag::ScopedPerfProbe perf("draft.enqueueSnapshot", draftQueuePerf());
  if (key.isEmpty() || session.isAsyncParseInProgress() || !session.document().isModified()) return false;
  const auto revision = session.document().revision();
  std::lock_guard lock(state_->mutex);
  auto& entry = state_->entries[key];
  if (!entry.clean &&
      ((entry.pending && entry.pending->revision == revision) || (entry.running && entry.runningRevision == revision)))
    return false;
  if (!entry.generation) entry.generation = ++state_->nextGeneration;
  entry.path = session.filePath();
  // QString/QVector storage is implicitly shared; piece and prefix vectors are
  // copied. No full-document materialization or newline/word-index scan here.
  entry.pending = State::Request{session.markdownText(), entry.path, revision, entry.generation};
  state_->enqueue(key, entry);
  return true;
}

void DraftSnapshotWriter::markClean(const QString& sourcePath, const QString& key) {
  if (key.isEmpty()) return;
  std::lock_guard lock(state_->mutex);
  auto& entry = state_->entries[key];
  entry.generation = ++state_->nextGeneration;
  entry.pending.reset();
  entry.path = sourcePath;
  entry.clean = true;
  state_->enqueue(key, entry);
}

void DraftSnapshotWriter::waitForDone() {
  std::unique_lock lock(state_->mutex);
  state_->changed.wait(lock, [this] {
    if (!state_->ready.empty()) return false;
    for (const auto& entry : state_->entries)
      if (entry.running) return false;
    return true;
  });
}

DraftSnapshotWriter::QueueStats DraftSnapshotWriter::queueStats() const {
  std::lock_guard lock(state_->mutex);
  QueueStats stats;
  for (const auto& entry : state_->entries) {
    if (entry.pending) {
      ++stats.pending;
      stats.pendingCodeUnits += entry.pending->text.size();
    }
    if (entry.runningRevision) {
      ++stats.running;
      stats.runningCodeUnits += entry.runningCodeUnits;
    }
  }
  return stats;
}
}  // namespace muffin

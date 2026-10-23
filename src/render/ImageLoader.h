#pragma once

#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QString>
#include <QFileSystemWatcher>
#include <QNetworkReply>

#include <memory>

#include "Export.h"

namespace muffin {
namespace diag {
struct MemoryAccounting;
}

/// Shared image cache with asynchronous remote loading and local file watching.
/// Publishes paint/geometry versions and notifies via imageReady().
class MUFFIN_UI_EXPORT ImageLoader : public QObject {
  friend struct diag::MemoryAccounting;
  Q_OBJECT

public:
  static ImageLoader& instance();

  /// Return a cached image for the given URL, or a null QImage if not available.
  QImage cached(const QString& url) const;
  // Shared local/data/remote lookup. Local files are decoded once and watched;
  // remote requests keep their existing asynchronous behavior.
  QImage image(const QString& url);
  void store(const QString& url, QImage image);
  // Retain while a layout uses the resource (including a pending download).
  // The final lease releases pixels, watches and any unfinished request.
  std::shared_ptr<void> retain(const QString& url);
  void setCacheLimits(qint64 bytes, qsizetype entries);
  struct CacheStats {
    qint64 bytes;
    qsizetype entries, users, watchedFiles, watchedDirectories, requests;
  };
  CacheStats cacheStats() const;

  /// Return true if the URL has been requested and is still downloading.
  bool isPending(const QString& url) const;

  /// Start an async download for a remote URL. Does nothing if already cached or pending.
  void request(const QString& url);

signals:
  /// Emitted when decoded image content changes, including failed downloads.
  void imageReady(QString url);

private:
  explicit ImageLoader(QObject* parent = nullptr);
  struct Usage;
  void scheduleRelease();
  void remove(const QString& url);
  void trim();
  void shutdown();

  QNetworkAccessManager* network_ = nullptr; // Owned; torn down on aboutToQuit (see .cpp).
  QHash<QString, QImage> cache_;
  QHash<QString, QNetworkReply*> pending_;
  QHash<QString, std::weak_ptr<void>> users_;
  mutable QHash<QString, quint64> recency_;
  mutable quint64 clock_ = 0;
  qint64 cacheBytes_ = 0;
  qint64 byteLimit_ = 32 * 1024 * 1024;
  qsizetype entryLimit_ = 256;
  bool releaseQueued_ = false;
  QFileSystemWatcher files_;
  QHash<QString, QSet<QString>> localFiles_;
  QHash<QString, QByteArray> fileIdentities_;
};

} // namespace muffin

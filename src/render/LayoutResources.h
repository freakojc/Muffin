#pragma once

#include "Export.h"
#include <QHash>
#include <QRecursiveMutex>
#include <QObject>
#include <QByteArray>
#include <memory>

namespace muffin {
struct LayoutResourceState;
struct LayoutResourceVersion {
  quint64 paint = 0, geometry = 0;
  // A layout keeps the small version state alive even when its producer evicts
  // the payload. Cache eviction alone must not invalidate an already built layout.
  std::shared_ptr<LayoutResourceState> state;
  bool operator==(const LayoutResourceVersion& other) const {
    return paint == other.paint && geometry == other.geometry;
  }
};
using LayoutResourceDependencies = QHash<QString, LayoutResourceVersion>;

// GUI resource producers publish immutable metric identities. Paint changes do
// not invalidate measurements; geometry changes do. Reads collect dependencies
// on the calling thread, including dependencies consumed from a cached child.
class MUFFIN_CORE_EXPORT LayoutResources : public QObject {
  Q_OBJECT
public:
  static LayoutResources& instance();
  static QString fontKey() { return QStringLiteral("fonts"); }
  static QString imageKey(const QString& url) { return QStringLiteral("image:") + url; }
  LayoutResourceVersion read(const QString& key) const;
  void record(const LayoutResourceDependencies& dependencies) const;
  bool matches(const LayoutResourceDependencies& dependencies, bool includePaint = true) const;
  void publish(const QString& key, const QByteArray& metrics);
  // Release producer ownership. Invalidate on content reset, preserve versions
  // on mere cache eviction. Live layout dependencies retain their version state.
  void release(const QString& key, bool invalidate = true);
signals:
  void resourceChanged(QString key);

private:
  LayoutResources();
  std::shared_ptr<LayoutResourceState> stateForKey(const QString& key) const; // mutex_ held
  // A last shared state can be released inside a registry operation; its deleter
  // removes the weak index entry under the same lock.
  mutable QRecursiveMutex mutex_;
  quint64 revision_ = 0;
  QHash<QString, std::shared_ptr<LayoutResourceState>> producers_;
  mutable QHash<QString, std::weak_ptr<LayoutResourceState>> entries_;
};

class MUFFIN_CORE_EXPORT LayoutResourceScope {
public:
  LayoutResourceScope();
  ~LayoutResourceScope();
  LayoutResourceScope(const LayoutResourceScope&) = delete;
  LayoutResourceScope& operator=(const LayoutResourceScope&) = delete;
  LayoutResourceDependencies dependencies;
};
} // namespace muffin

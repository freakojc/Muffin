#include "render/LayoutResources.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMutexLocker>
#include <vector>

namespace muffin {
struct LayoutResourceState {
  quint64 paint = 0, geometry = 0;
  QByteArray metrics;
};
namespace {
thread_local std::vector<LayoutResourceScope*> scopes;
}
LayoutResources& LayoutResources::instance() {
  // Like ImageLoader, this may outlive QApplication in a shared-library build.
  static auto* resources = new LayoutResources();
  return *resources;
}
LayoutResources::LayoutResources() {
  if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
    connect(app, &QGuiApplication::fontDatabaseChanged, this, [this] {
      const auto version = read(fontKey());
      publish(fontKey(), QByteArray::number(version.geometry + 1));
    });
}
LayoutResourceVersion LayoutResources::read(const QString& key) const {
  LayoutResourceVersion version;
  {
    QMutexLocker lock(&mutex_);
    auto state = stateForKey(key);
    version = {state->paint, state->geometry, std::move(state)};
  }
  for (auto* scope : scopes)
    scope->dependencies.insert(key, version);
  return version;
}
void LayoutResources::record(const LayoutResourceDependencies& dependencies) const {
  for (auto* scope : scopes)
    for (auto it = dependencies.cbegin(); it != dependencies.cend(); ++it)
      scope->dependencies.insert(it.key(), it.value());
}
bool LayoutResources::matches(const LayoutResourceDependencies& dependencies, bool includePaint) const {
  QMutexLocker lock(&mutex_);
  for (auto it = dependencies.cbegin(); it != dependencies.cend(); ++it) {
    const auto state = it->state ? it->state : entries_.value(it.key()).lock();
    if ((state ? state->geometry : 0) != it->geometry || (includePaint && (state ? state->paint : 0) != it->paint))
      return false;
  }
  return true;
}
std::shared_ptr<LayoutResourceState> LayoutResources::stateForKey(const QString& key) const {
  auto state = entries_.value(key).lock();
  if (!state) {
    state = std::shared_ptr<LayoutResourceState>(new LayoutResourceState, [this, key](LayoutResourceState* value) {
      delete value;
      QMutexLocker lock(&mutex_);
      if (entries_.value(key).expired())
        entries_.remove(key);
    });
    entries_.insert(key, state);
  }
  return state;
}
void LayoutResources::publish(const QString& key, const QByteArray& metrics) {
  {
    QMutexLocker lock(&mutex_);
    auto state = stateForKey(key);
    producers_.insert(key, state);
    const auto revision = ++revision_;
    if (state->paint == 0 || state->metrics != metrics)
      state->geometry = revision;
    state->paint = revision;
    state->metrics = metrics;
  }
  emit resourceChanged(key);
}
void LayoutResources::release(const QString& key, bool invalidate) {
  {
    QMutexLocker lock(&mutex_);
    auto state = entries_.value(key).lock();
    producers_.remove(key);
    if (state && invalidate) {
      state->paint = state->geometry = ++revision_;
      state->metrics.clear();
    }
    if (!state || state.use_count() == 1)
      entries_.remove(key);
  }
  if (invalidate)
    emit resourceChanged(key);
}
LayoutResourceScope::LayoutResourceScope() {
  scopes.push_back(this);
}
LayoutResourceScope::~LayoutResourceScope() {
  scopes.pop_back();
}
} // namespace muffin

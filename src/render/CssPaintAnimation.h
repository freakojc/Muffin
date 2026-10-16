#pragma once

#include "theme/CssComputedStyleEngine.h"
#include "theme/ThemeDefinition.h"
#include "document/NodeId.h"

#include <array>
#include <functional>
#include <memory>

namespace muffin {

struct CssPaintTransition {
  QString property, easing = QStringLiteral("ease");
  qreal durationMs = 0, delayMs = 0;
};
struct CssPaintKeyframe {
  qreal position = 0;
  ThemeElementStyle style;
  QString easing;
};
struct CssPaintAnimation {
  QString name, easing = QStringLiteral("ease"), direction = QStringLiteral("normal"), fill = QStringLiteral("none");
  qreal durationMs = 0, delayMs = 0, iterations = 1;
  bool paused = false;
  QHash<QString, QVector<CssPaintKeyframe>> tracks;
};

// Immutable recipes are resolved during layout. Paint samples typed values and
// never evaluates selectors, parses CSS, or changes text shaping/line geometry.
struct CssPaintRecipe {
  quint64 fingerprint = 0;
  std::array<ThemeElementStyle, 8> states;  // hover, focus, active bits
  std::array<QVector<CssPaintTransition>, 8> transitions;
  std::array<QVector<CssPaintAnimation>, 8> animations;
};

std::shared_ptr<const CssPaintRecipe> compileCssPaintRecipe(
    const QString& key, const std::function<CssComputedStyle(CssElementState)>& resolve,
    const CssThemeSheet& sheet, bool reducedMotion = false, quint8 stateMask = 7);
qreal cssTimingProgress(qreal progress, const QString& easing);
void interpolateCssPaintProperty(ThemeElementStyle& result, const ThemeElementStyle& from,
                                 const ThemeElementStyle& to, const QString& property, qreal progress);

class CssPaintContext {
 public:
  // Pointer and geometry share painter/view coordinates. A missing pointer
  // clears hover while the timelines continue to paint their outgoing fades.
  std::optional<QPointF> pointer;
  bool pressed = false;
  qreal timeMs = -1;  // negative = deterministic static rendering
  bool needsFrame = false;
  QSet<NodeId> focusedAncestors;
  ThemeElementStyle sample(const CssPaintRecipe& recipe, const QString& identity, int state);
  void clear();

 private:
  struct RunningAnimation { qreal started = 0, pausedAt = -1; };
  struct Timeline {
    quint64 fingerprint = 0;
    int state = 0;
    qreal changedAt = 0, lastSeen = 0;
    ThemeElementStyle from;
    QHash<QString, RunningAnimation> animations;
  };
  QHash<QString, Timeline> timelines_;
  qreal nextPruneMs_ = 0;
};

}  // namespace muffin

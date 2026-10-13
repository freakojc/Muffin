#include "render/CssPaintAnimation.h"

#include "theme/CssThemeMapper.h"
#include "theme/CssValueParser.h"

#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace muffin {
namespace {
const QStringList& properties() {
  static const QStringList names{"color", "background-color", "background-image", "background-position-x", "background-position-y",
      "background-size", "opacity", "transform", "box-shadow", "text-shadow", "-webkit-text-stroke", "filter", "border-color", "border-radius", "width", "height"};
  return names;
}
bool affects(const QString& rule, const QString& property) {
  return rule == "all" || rule == property || (rule == "background-position" && property.startsWith("background-position-")) ||
         (rule == "background" && property.startsWith("background-"));
}
std::optional<qreal> milliseconds(QString token) {
  qreal multiplier = 1;
  if (token.endsWith("ms")) token.chop(2);
  else if (token.endsWith('s')) { token.chop(1); multiplier = 1000; }
  else return {};
  bool ok = false; const qreal value = token.toDouble(&ok) * multiplier;
  return ok && std::isfinite(value) ? std::optional<qreal>(value) : std::nullopt;
}
bool timing(const QString& token) {
  return QStringList{"linear", "ease", "ease-in", "ease-out", "ease-in-out", "step-start", "step-end"}.contains(token) ||
         token.startsWith("cubic-bezier(") || token.startsWith("steps(");
}
QString listItem(const CssComputedStyle& style, const QString& name, int index, const QString& fallback) {
  const auto raw = style.resolvedValue(name);
  const auto list = CssThemeParser::splitTopLevelCommas(raw);
  return raw.isEmpty() || list.isEmpty() ? fallback : list[index % list.size()].trimmed();
}
QVector<CssPaintTransition> transitions(const CssComputedStyle& style) {
  QVector<CssPaintTransition> result;
  const auto shorthand = style.resolvedValue("transition");
  for (const auto& item : CssThemeParser::splitTopLevelCommas(shorthand)) {
    CssPaintTransition track; track.property = "all";
    int times = 0;
    for (const auto& token : splitTopLevelSpaces(item)) {
      if (const auto time = milliseconds(token)) (times++ == 0 ? track.durationMs : track.delayMs) = *time;
      else if (timing(token)) track.easing = token;
      else track.property = token;
    }
    result.push_back(track);
  }
  if (result.isEmpty()) result.push_back({"all"});
  const auto explicitNames = style.resolvedValue("transition-property");
  const int count = explicitNames.isEmpty() ? result.size() : CssThemeParser::splitTopLevelCommas(explicitNames).size();
  QVector<CssPaintTransition> expanded;
  for (int i = 0; i < count; ++i) {
    auto track = result[i % result.size()];
    track.property = listItem(style, "transition-property", i, track.property);
    track.durationMs = milliseconds(listItem(style, "transition-duration", i, QString::number(track.durationMs) + "ms")).value_or(0);
    track.delayMs = milliseconds(listItem(style, "transition-delay", i, QString::number(track.delayMs) + "ms")).value_or(0);
    track.easing = listItem(style, "transition-timing-function", i, track.easing);
    if (track.property != "none" && track.durationMs > 0) expanded.push_back(track);
  }
  return expanded;
}
QVector<CssPaintAnimation> animations(const QString& key, const CssComputedStyle& style, const CssThemeSheet& sheet) {
  QVector<CssPaintAnimation> parsed;
  for (const auto& item : CssThemeParser::splitTopLevelCommas(style.resolvedValue("animation"))) {
    CssPaintAnimation animation;
    int times = 0;
    for (const auto& token : splitTopLevelSpaces(item)) {
      bool number = false; const auto count = token.toDouble(&number);
      if (const auto time = milliseconds(token)) (times++ == 0 ? animation.durationMs : animation.delayMs) = *time;
      else if (timing(token)) animation.easing = token;
      else if (token == "infinite") animation.iterations = -1;
      else if (number) animation.iterations = qMax<qreal>(0, count);
      else if (QStringList{"normal", "reverse", "alternate", "alternate-reverse"}.contains(token)) animation.direction = token;
      else if (QStringList{"forwards", "backwards", "both"}.contains(token)) animation.fill = token;
      else if (token == "paused" || token == "running") animation.paused = token == "paused";
      else animation.name = token;
    }
    parsed.push_back(animation);
  }
  if (parsed.isEmpty()) parsed.push_back({});
  const auto names = style.resolvedValue("animation-name");
  const int count = names.isEmpty() ? parsed.size() : CssThemeParser::splitTopLevelCommas(names).size();
  QVector<CssPaintAnimation> result;
  for (int i = 0; i < count; ++i) {
    auto animation = parsed[i % parsed.size()];
    animation.name = listItem(style, "animation-name", i, animation.name);
    animation.easing = listItem(style, "animation-timing-function", i, animation.easing);
    animation.direction = listItem(style, "animation-direction", i, animation.direction);
    animation.fill = listItem(style, "animation-fill-mode", i, animation.fill);
    animation.paused = listItem(style, "animation-play-state", i, animation.paused ? "paused" : "running") == "paused";
    animation.durationMs = milliseconds(listItem(style, "animation-duration", i, QString::number(animation.durationMs) + "ms")).value_or(0);
    animation.delayMs = milliseconds(listItem(style, "animation-delay", i, QString::number(animation.delayMs) + "ms")).value_or(0);
    const auto iterations = listItem(style, "animation-iteration-count", i, QString::number(animation.iterations));
    animation.iterations = iterations == "infinite" ? -1 : iterations.toDouble();
    if (animation.name.isEmpty() || animation.name == "none" || animation.durationMs <= 0 || animation.iterations == 0) continue;
    // Last definition wins. Variables are resolved against this element, not
    // against a load-time prototype or global theme variable map.
    const CssKeyframes* definition = nullptr;
    for (const auto& candidate : sheet.keyframes()) if (candidate.name == animation.name) definition = &candidate;
    if (!definition) continue;
    for (const auto& stop : definition->stops) {
      const auto computed = cssKeyframeStyle(style, stop.declarations);
      QSet<QString> authored;
      QString easing;
      for (const auto& declaration : stop.declarations) {
        if (declaration.important) continue;  // invalid inside @keyframes
        if (declaration.property == "animation-timing-function") easing = declaration.value;
        for (const auto& property : properties()) if (affects(declaration.property, property)) authored.insert(property);
      }
      const auto projected = CssThemeMapper::projectComputedStyle(key, computed);
      for (const auto& property : authored) animation.tracks[property].push_back({stop.position, projected, easing});
    }
    const auto underlying = CssThemeMapper::projectComputedStyle(key, style);
    for (auto it = animation.tracks.begin(); it != animation.tracks.end(); ++it) {
      auto& stops = it.value();
      std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.position < b.position; });
      // Merge same-offset declarations with the later one taking precedence.
      for (int j = stops.size() - 1; j > 0; --j) if (stops[j].position == stops[j - 1].position) stops.removeAt(j - 1);
      if (stops.front().position > 0) stops.prepend({0, underlying, {}});
      if (stops.back().position < 1) stops.append({1, underlying, {}});
    }
    if (!animation.tracks.isEmpty()) result.push_back(std::move(animation));
  }
  return result;
}
qreal mix(qreal a, qreal b, qreal t) { return a + (b - a) * t; }
QColor mix(QColor a, QColor b, qreal t) {
  if (a == b) return a;
  if (!a.isValid()) a = QColor(b.red(), b.green(), b.blue(), 0);
  if (!b.isValid()) b = QColor(a.red(), a.green(), a.blue(), 0);
  const auto alpha = mix(a.alphaF(), b.alphaF(), t);
  if (alpha <= 0) return QColor(Qt::transparent);
  // Premultiplied interpolation prevents transparent black creating dark halos.
  return QColor::fromRgbF(qBound<qreal>(0, mix(a.redF() * a.alphaF(), b.redF() * b.alphaF(), t) / alpha, 1),
                         qBound<qreal>(0, mix(a.greenF() * a.alphaF(), b.greenF() * b.alphaF(), t) / alpha, 1),
                         qBound<qreal>(0, mix(a.blueF() * a.alphaF(), b.blueF() * b.alphaF(), t) / alpha, 1), qBound<qreal>(0, alpha, 1));
}
CssLengthPercentage mix(CssLengthPercentage a, CssLengthPercentage b, qreal t) {
  if (a.status != CssLengthStatus::Valid || b.status != CssLengthStatus::Valid) return t < .5 ? a : b;
  return {CssLengthStatus::Valid, mix(a.px, b.px, t), mix(a.fraction, b.fraction, t), a.hasPercentage || b.hasPercentage};
}
bool equalProperty(const ThemeElementStyle& a, const ThemeElementStyle& b, const QString& property) {
  auto projected = a;
  interpolateCssPaintProperty(projected, a, b, property, 1);
  return projected.paint == a.paint && projected.text.textShadow == a.text.textShadow && projected.text.strokeWidth == a.text.strokeWidth &&
         projected.text.strokeColor == a.text.strokeColor && projected.box.widthLength == a.box.widthLength &&
         projected.box.heightLength == a.box.heightLength && projected.box.borderRadius == a.box.borderRadius &&
         projected.box.borderTopColor == a.box.borderTopColor && projected.box.borderRightColor == a.box.borderRightColor &&
         projected.box.borderBottomColor == a.box.borderBottomColor && projected.box.borderLeftColor == a.box.borderLeftColor;
}
ThemeElementStyle transitionSample(const CssPaintRecipe& recipe, int state, const ThemeElementStyle& from, qreal elapsed, bool& pending) {
  auto result = recipe.states[state];
  for (const auto& property : properties()) {
    const CssPaintTransition* match = nullptr;
    for (const auto& transition : recipe.transitions[state]) if (affects(transition.property, property)) match = &transition;
    if (!match || equalProperty(from, result, property)) continue;
    const auto raw = (elapsed - match->delayMs) / match->durationMs;
    if (raw < 1) pending = true;
    interpolateCssPaintProperty(result, from, recipe.states[state], property, cssTimingProgress(qBound<qreal>(0, raw, 1), match->easing));
  }
  return result;
}
}  // namespace

qreal cssTimingProgress(qreal x, const QString& easing) {
  x = qBound<qreal>(0, x, 1);
  if (easing == "linear") return x;
  if (easing == "step-start") return 1;
  if (easing == "step-end") return x == 1 ? 1 : 0;
  if (easing.startsWith("steps(")) {
    auto args = easing.mid(6); args.chop(1);
    const auto parts = CssThemeParser::splitTopLevelCommas(args);
    const int count = parts.isEmpty() ? 1 : qMax(1, parts[0].toInt());
    const bool start = parts.size() > 1 && (parts[1].trimmed() == "start" || parts[1].trimmed() == "jump-start");
    return qMin<qreal>(1, (std::floor(x * count) + (start ? 1 : 0)) / count);
  }
  qreal x1 = .25, y1 = .1, x2 = .25, y2 = 1;
  if (easing == "ease-in") x1 = .42, y1 = 0, x2 = 1;
  else if (easing == "ease-out") x1 = 0, y1 = 0, x2 = .58;
  else if (easing == "ease-in-out") x1 = .42, y1 = 0, x2 = .58;
  else if (easing.startsWith("cubic-bezier(")) {
    auto args = easing.mid(13); args.chop(1);
    const auto parts = CssThemeParser::splitTopLevelCommas(args);
    if (parts.size() != 4) return x;
    x1 = parts[0].toDouble(); y1 = parts[1].toDouble(); x2 = parts[2].toDouble(); y2 = parts[3].toDouble();
    if (x1 < 0 || x1 > 1 || x2 < 0 || x2 > 1) return x;
  }
  if (x == 0 || x == 1) return x;
  const auto bezier = [](qreal t, qreal a, qreal b) { return 3 * (1 - t) * (1 - t) * t * a + 3 * (1 - t) * t * t * b + t * t * t; };
  qreal low = 0, high = 1;
  for (int i = 0; i < 24; ++i) { const qreal t = (low + high) / 2; (bezier(t, x1, x2) < x ? low : high) = t; }
  return bezier((low + high) / 2, y1, y2);
}

void interpolateCssPaintProperty(ThemeElementStyle& r, const ThemeElementStyle& a, const ThemeElementStyle& b, const QString& p, qreal t) {
  auto& v = r.paint; const auto& x = a.paint; const auto& y = b.paint;
  if (p == "color") v.color = mix(x.color, y.color, t);
  else if (p == "background-color") v.backgroundColor = mix(x.backgroundColor, y.backgroundColor, t);
  else if (p == "background-position-x") v.backgroundPositionX = mix(x.backgroundPositionX, y.backgroundPositionX, t);
  else if (p == "background-position-y") v.backgroundPositionY = mix(x.backgroundPositionY, y.backgroundPositionY, t);
  else if (p == "background-size") { v.backgroundSizeX = mix(x.backgroundSizeX, y.backgroundSizeX, t); v.backgroundSizeY = mix(x.backgroundSizeY, y.backgroundSizeY, t); }
  else if (p == "background-image") {
    v.backgroundImage = t < .5 ? x.backgroundImage : y.backgroundImage;
    if (x.backgroundImage.kind == y.backgroundImage.kind && x.backgroundImage.stops.size() == y.backgroundImage.stops.size()) {
      v.backgroundImage.angleDeg = mix(x.backgroundImage.angleDeg, y.backgroundImage.angleDeg, t);
      for (size_t i = 0; i < v.backgroundImage.stops.size(); ++i) {
        v.backgroundImage.stops[i].color = mix(x.backgroundImage.stops[i].color, y.backgroundImage.stops[i].color, t);
        v.backgroundImage.stops[i].position = mix(x.backgroundImage.stops[i].position, y.backgroundImage.stops[i].position, t);
      }
    }
  } else if (p == "opacity") v.opacity = mix(x.opacity, y.opacity, t);
  else if (p == "transform") {
    v.transformScale = mix(x.transformScale, y.transformScale, t);
    auto first = x.transforms, last = y.transforms;
    const auto identity = [](auto ops) {
      for (auto& op : ops) {
        op.angle = 0;
        op.x = op.y = {CssLengthStatus::Valid, op.kind == ThemeElementPaintStyle::Transform::Kind::Scale ? 1. : 0.};
      }
      return ops;
    };
    if (first.empty()) first = identity(last);
    if (last.empty()) last = identity(first);
    v.transforms = t < .5 ? first : last;
    if (first.size() == last.size()) for (size_t i = 0; i < first.size(); ++i) if (first[i].kind == last[i].kind) {
      v.transforms[i].x = mix(first[i].x, last[i].x, t); v.transforms[i].y = mix(first[i].y, last[i].y, t);
      v.transforms[i].angle = mix(first[i].angle, last[i].angle, t);
    }
  } else if (p == "box-shadow") {
    v.boxShadowColor = mix(x.boxShadowColor, y.boxShadowColor, t); v.boxShadowBlur = mix(x.boxShadowBlur, y.boxShadowBlur, t);
    v.boxShadowOffsetX = mix(x.boxShadowOffsetX, y.boxShadowOffsetX, t); v.boxShadowOffsetY = mix(x.boxShadowOffsetY, y.boxShadowOffsetY, t);
    v.boxShadowSpread = mix(x.boxShadowSpread, y.boxShadowSpread, t);
  } else if (p == "text-shadow") {
    auto& shadow = r.text.textShadow; const auto& s = a.text.textShadow; const auto& e = b.text.textShadow;
    shadow.present = s.present || e.present; shadow.color = mix(s.color, e.color, t);
    shadow.offset = s.offset + (e.offset - s.offset) * t; shadow.blur = mix(s.blur, e.blur, t);
  } else if (p == "-webkit-text-stroke") {
    r.text.strokeWidth = mix(a.text.strokeWidth, b.text.strokeWidth, t); r.text.strokeColor = mix(a.text.strokeColor, b.text.strokeColor, t);
  } else if (p == "filter") {
    v.filterPresent = x.filterPresent || y.filterPresent;
    v.filterBlur = mix(x.filterBlur, y.filterBlur, t); v.filterBrightness = mix(x.filterBrightness, y.filterBrightness, t);
    v.filterContrast = mix(x.filterContrast, y.filterContrast, t); v.filterGrayscale = mix(x.filterGrayscale, y.filterGrayscale, t);
    v.filterSepia = mix(x.filterSepia, y.filterSepia, t); v.filterHueRotateDeg = mix(x.filterHueRotateDeg, y.filterHueRotateDeg, t);
    v.filterOpacity = mix(x.filterOpacity, y.filterOpacity, t);
  } else if (p == "border-color") {
    r.box.borderTopColor = mix(a.box.borderTopColor, b.box.borderTopColor, t); r.box.borderRightColor = mix(a.box.borderRightColor, b.box.borderRightColor, t);
    r.box.borderBottomColor = mix(a.box.borderBottomColor, b.box.borderBottomColor, t); r.box.borderLeftColor = mix(a.box.borderLeftColor, b.box.borderLeftColor, t);
  } else if (p == "border-radius") r.box.borderRadius = mix(a.box.borderRadius, b.box.borderRadius, t);
  else if (p == "width") r.box.widthLength = mix(a.box.widthLength, b.box.widthLength, t);
  else if (p == "height") r.box.heightLength = mix(a.box.heightLength, b.box.heightLength, t);
}

std::shared_ptr<const CssPaintRecipe> compileCssPaintRecipe(const QString& key,
    const std::function<CssComputedStyle(CssElementState)>& resolve, const CssThemeSheet& sheet, bool reducedMotion) {
  auto recipe = std::make_shared<CssPaintRecipe>();
  bool changes = false;
  for (int bits = 0; bits < 8; ++bits) {
    CssElementState state; state.hover = bits & 1; state.focus = bits & 2; state.mdFocus = state.focus; state.active = bits & 4;
    const auto computed = resolve(state);
    recipe->states[bits] = CssThemeMapper::projectComputedStyle(key, computed);
    recipe->fingerprint = qHashMulti(size_t(recipe->fingerprint), computed.fingerprint());
    if (!reducedMotion) { recipe->transitions[bits] = transitions(computed); recipe->animations[bits] = animations(key, computed, sheet); }
    changes = changes || !recipe->animations[bits].isEmpty() || recipe->states[bits].layout.display != recipe->states[0].layout.display;
    for (const auto& property : properties()) changes = changes || !equalProperty(recipe->states[bits], recipe->states[0], property);
  }
  return changes ? recipe : nullptr;
}

ThemeElementStyle CssPaintContext::sample(const CssPaintRecipe& recipe, const QString& identity, int state) {
  state &= 7;
  if (timeMs < 0) return recipe.states[state];
  auto found = timelines_.find(identity);
  if (found == timelines_.end() || found->fingerprint != recipe.fingerprint) {
    Timeline timeline; timeline.fingerprint = recipe.fingerprint; timeline.from = recipe.states[0]; timeline.changedAt = timeMs;
    found = timelines_.insert(identity, std::move(timeline));
  }
  auto& timeline = found.value();
  timeline.lastSeen = timeMs;
  if (timeline.state != state) {
    bool unused = false;
    timeline.from = transitionSample(recipe, timeline.state, timeline.from, timeMs - timeline.changedAt, unused);
    timeline.state = state; timeline.changedAt = timeMs;
  }
  bool pending = false;
  auto result = transitionSample(recipe, state, timeline.from, timeMs - timeline.changedAt, pending);
  QSet<QString> active;
  int index = 0;
  for (const auto& animation : recipe.animations[state]) {
    const auto name = animation.name + '/' + QString::number(index++);
    active.insert(name);
    if (!timeline.animations.contains(name)) timeline.animations.insert(name, {timeMs, -1});
    auto& running = timeline.animations[name];
    if (animation.paused && running.pausedAt < 0) running.pausedAt = timeMs;
    else if (!animation.paused && running.pausedAt >= 0) { running.started += timeMs - running.pausedAt; running.pausedAt = -1; }
    const qreal elapsed = (running.pausedAt >= 0 ? running.pausedAt : timeMs) - running.started - animation.delayMs;
    const bool before = elapsed < 0;
    const bool after = animation.iterations >= 0 && elapsed >= animation.durationMs * animation.iterations;
    if (!animation.paused && !after) pending = true;
    if ((before && animation.fill != "backwards" && animation.fill != "both") ||
        (after && animation.fill != "forwards" && animation.fill != "both")) continue;
    const qreal iterations = before ? 0 : after ? animation.iterations : elapsed / animation.durationMs;
    auto cycle = qint64(std::floor(iterations));
    qreal phase = iterations - cycle;
    if (after && phase == 0 && cycle > 0) { --cycle; phase = 1; }
    const bool reverse = animation.direction == "reverse" ||
        (animation.direction == "alternate" && cycle % 2) || (animation.direction == "alternate-reverse" && !(cycle % 2));
    if (reverse) phase = 1 - phase;
    for (auto it = animation.tracks.cbegin(); it != animation.tracks.cend(); ++it) {
      const auto& stops = it.value();
      auto upper = std::upper_bound(stops.cbegin(), stops.cend(), phase, [](qreal value, const auto& stop) { return value < stop.position; });
      const auto& to = upper == stops.cend() ? stops.back() : *upper;
      const auto& from = upper == stops.cbegin() ? stops.front() : *(upper - 1);
      const qreal local = to.position == from.position ? 1 : (phase - from.position) / (to.position - from.position);
      interpolateCssPaintProperty(result, from.style, to.style, it.key(),
                                  cssTimingProgress(local, from.easing.isEmpty() ? animation.easing : from.easing));
    }
  }
  for (auto it = timeline.animations.begin(); it != timeline.animations.end();)
    if (!active.contains(it.key())) it = timeline.animations.erase(it); else ++it;
  needsFrame = needsFrame || pending;
  // Retain offscreen timelines for smooth re-entry, with a bounded view-owned
  // cache rather than one allocation for every block in a large document.
  if (timelines_.size() > 4096 && timeMs >= nextPruneMs_) {
    nextPruneMs_ = timeMs + 1000;
    QVector<QPair<qreal, QString>> oldest;
    oldest.reserve(timelines_.size());
    for (auto it = timelines_.cbegin(); it != timelines_.cend(); ++it) oldest.append({it->lastSeen, it.key()});
    std::sort(oldest.begin(), oldest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (int i = 0, count = oldest.size() - 4096; i < count; ++i) timelines_.remove(oldest[i].second);
  }
  return result;
}
void CssPaintContext::clear() { timelines_.clear(); needsFrame = false; nextPruneMs_ = 0; }

}  // namespace muffin

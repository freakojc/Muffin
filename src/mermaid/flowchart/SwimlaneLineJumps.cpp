#include "mermaid/flowchart/SwimlaneLineJumps.h"

#include <QHash>
#include <algorithm>
#include <cmath>
#include <optional>
#include <tuple>

namespace muffin::mermaid::flowchart {
namespace {
constexpr qreal kEps = 1e-6;
constexpr qreal kRoundedCornerRadius = 5.0;
constexpr qreal kCornerEpsilon = 1e-5;
constexpr qreal kMinimumJumpRadius = 1e-3;

struct LineJumpCrossing {
  int edgeIndex = -1;
  int segmentIndex = -1;
  qreal t = 0.0;
  QPointF point;
};

struct RoundedCorner {
  QPointF start;
  QPointF end;
  QPointF control;
  qreal cutLength = 0.0;
};

QString jumpNumber(qreal value) {
  qreal rounded = std::floor(value * 1000.0 + 0.5) / 1000.0;
  if (std::abs(rounded) < 0.0005) rounded = 0.0;
  QString text = QString::number(rounded, 'f', 3);
  while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0')))
    text.chop(1);
  if (text.endsWith(QLatin1Char('.'))) text.chop(1);
  return text;
}

QString jumpPoint(const QPointF& point) {
  return jumpNumber(point.x()) + QLatin1Char(',') + jumpNumber(point.y());
}

bool horizontalSegment(const QPointF& a, const QPointF& b) {
  return std::abs(b.x() - a.x()) >= std::abs(b.y() - a.y());
}

std::optional<std::tuple<QPointF, qreal, qreal>> segmentIntersection(
    const QPointF& a1, const QPointF& a2,
    const QPointF& b1, const QPointF& b2) {
  const qreal dxA = a2.x() - a1.x();
  const qreal dyA = a2.y() - a1.y();
  const qreal dxB = b2.x() - b1.x();
  const qreal dyB = b2.y() - b1.y();
  const qreal denominator = dxA * dyB - dyA * dxB;
  if (denominator == 0.0) return std::nullopt;
  const qreal dx = b1.x() - a1.x();
  const qreal dy = b1.y() - a1.y();
  const qreal tA = (dx * dyB - dy * dxB) / denominator;
  const qreal tB = (dx * dyA - dy * dxA) / denominator;
  if (tA <= kEps || tA >= 1.0 - kEps ||
      tB <= kEps || tB >= 1.0 - kEps)
    return std::nullopt;
  return std::tuple<QPointF, qreal, qreal>(
      QPointF(a1.x() + tA * dxA, a1.y() + tA * dyA), tA, tB);
}

QVector<LineJumpCrossing> lineJumpCrossings(
    const QVector<FlowLayoutEdge>& edges) {
  QVector<LineJumpCrossing> crossings;
  for (int i = 0; i < edges.size(); ++i) {
    const QVector<QPointF>& a = edges.at(i).points;
    for (int j = i + 1; j < edges.size(); ++j) {
      const QVector<QPointF>& b = edges.at(j).points;
      for (int ai = 0; ai + 1 < a.size(); ++ai) {
        for (int bi = 0; bi + 1 < b.size(); ++bi) {
          const auto hit = segmentIntersection(
              a.at(ai), a.at(ai + 1), b.at(bi), b.at(bi + 1));
          if (!hit) continue;
          const auto& [point, tA, tB] = *hit;
          const bool aHorizontal = horizontalSegment(a.at(ai), a.at(ai + 1));
          const bool bHorizontal = horizontalSegment(b.at(bi), b.at(bi + 1));
          if (aHorizontal != bHorizontal && aHorizontal)
            crossings.append({i, ai, tA, point});
          else
            crossings.append({j, bi, tB, point});
        }
      }
    }
  }
  return crossings;
}

std::optional<RoundedCorner> roundedCorner(
    const QPointF& previous, const QPointF& current,
    const QPointF& next) {
  const QPointF incoming = current - previous;
  const QPointF outgoing = next - current;
  const qreal incomingLength = std::hypot(incoming.x(), incoming.y());
  const qreal outgoingLength = std::hypot(outgoing.x(), outgoing.y());
  if (incomingLength < kCornerEpsilon || outgoingLength < kCornerEpsilon)
    return std::nullopt;
  const QPointF inUnit = incoming / incomingLength;
  const QPointF outUnit = outgoing / outgoingLength;
  const qreal dot = std::clamp(
      QPointF::dotProduct(inUnit, outUnit), -1.0, 1.0);
  const qreal angle = std::acos(dot);
  if (angle < kCornerEpsilon ||
      std::abs(M_PI - angle) < kCornerEpsilon)
    return std::nullopt;
  const qreal cut = std::min({
      kRoundedCornerRadius / std::sin(angle / 2.0),
      incomingLength / 2.0, outgoingLength / 2.0});
  return RoundedCorner{
      current - inUnit * cut, current + outUnit * cut, current, cut};
}

bool curveSupportsLineHops(const QString& curve) {
  return curve == QLatin1String("linear") ||
         curve == QLatin1String("rounded") ||
         curve == QLatin1String("step") ||
         curve == QLatin1String("stepBefore") ||
         curve == QLatin1String("stepAfter");
}

QString rewriteLineJumpPath(
    const FlowLayoutEdge& edge, const FlowEdge& semantic,
    const QString& curve, const QVector<LineJumpCrossing>& crossings,
    bool gapStyle) {
  QVector<QPointF> points = edge.points;
  clipFlowEdgeForMarkers(points, semantic.type);
  if (points.size() < 2) return {};

  struct Jump {
    qreal t = 0.0;
    QPointF point;
    qreal distance = 0.0;
    qreal radius = 6.0;
  };
  QHash<int, QVector<Jump>> bySegment;
  for (const LineJumpCrossing& crossing : crossings) {
    if (crossing.segmentIndex < 0 ||
        crossing.segmentIndex + 1 >= points.size())
      continue;
    const QPointF a = points.at(crossing.segmentIndex);
    const QPointF b = points.at(crossing.segmentIndex + 1);
    const qreal length = std::hypot(b.x() - a.x(), b.y() - a.y());
    bySegment[crossing.segmentIndex].append(
        {crossing.t, crossing.point, crossing.t * length, 6.0});
  }

  const bool rounded = curve == QLatin1String("rounded");
  QStringList parts{QStringLiteral("M") + jumpPoint(points.first())};
  for (int i = 0; i + 1 < points.size(); ++i) {
    const QPointF a = points.at(i);
    const QPointF b = points.at(i + 1);
    const QPointF delta = b - a;
    const qreal length = std::hypot(delta.x(), delta.y());
    const QPointF unit = length == 0.0 ? QPointF() : delta / length;
    const int sweep = horizontalSegment(a, b)
        ? (delta.x() >= 0.0 ? 1 : 0)
        : (delta.y() >= 0.0 ? 1 : 0);

    qreal consumedStart = 0.0;
    if (rounded && i > 0) {
      const auto corner = roundedCorner(points.at(i - 1), a, b);
      if (corner) consumedStart = corner->cutLength;
    }
    qreal segmentEnd = length;
    std::optional<RoundedCorner> upcoming;
    if (rounded && i + 2 < points.size()) {
      upcoming = roundedCorner(a, b, points.at(i + 2));
      if (upcoming) segmentEnd = length - upcoming->cutLength;
    }

    QVector<Jump> jumps = bySegment.value(i);
    std::sort(jumps.begin(), jumps.end(),
              [](const Jump& left, const Jump& right) {
                return left.t < right.t;
              });
    for (Jump& jump : jumps)
      jump.radius = std::min({jump.radius,
                              jump.distance - consumedStart,
                              segmentEnd - jump.distance});
    for (int j = 0; j + 1 < jumps.size(); ++j) {
      const qreal distance = jumps.at(j + 1).distance - jumps.at(j).distance;
      if (jumps.at(j).radius + jumps.at(j + 1).radius > distance) {
        jumps[j].radius = std::min(jumps.at(j).radius, distance / 2.0);
        jumps[j + 1].radius =
            std::min(jumps.at(j + 1).radius, distance / 2.0);
      }
    }
    for (const Jump& jump : jumps) {
      if (jump.radius < kMinimumJumpRadius) continue;
      const QPointF before = jump.point - unit * jump.radius;
      const QPointF after = jump.point + unit * jump.radius;
      parts.append(QStringLiteral("L") + jumpPoint(before));
      if (gapStyle)
        parts.append(QStringLiteral("M") + jumpPoint(after));
      else
        parts.append(QStringLiteral("A%1,%1 0 0 %2 %3")
                         .arg(jumpNumber(jump.radius))
                         .arg(sweep)
                         .arg(jumpPoint(after)));
    }
    if (rounded && upcoming) {
      parts.append(QStringLiteral("L") + jumpPoint(upcoming->start));
      parts.append(QStringLiteral("Q%1 %2")
                       .arg(jumpPoint(upcoming->control),
                            jumpPoint(upcoming->end)));
    } else {
      parts.append(QStringLiteral("L") + jumpPoint(b));
    }
  }
  return parts.join(QLatin1Char(' '));
}

}  // namespace

void applyLineJumps(FlowLayoutResult& result, const FlowchartData& data,
                    const SwimlaneLayoutOptions& options) {
  if (options.lineHops == QLatin1String("false")) return;
  const QVector<LineJumpCrossing> crossings = lineJumpCrossings(result.edges);
  if (crossings.isEmpty()) return;

  QHash<int, QVector<LineJumpCrossing>> byEdge;
  for (const LineJumpCrossing& crossing : crossings)
    byEdge[crossing.edgeIndex].append(crossing);
  QHash<QString, const FlowEdge*> semantics;
  for (const FlowEdge& edge : data.edges) semantics.insert(edge.id, &edge);

  for (auto it = byEdge.cbegin(); it != byEdge.cend(); ++it) {
    if (it.key() < 0 || it.key() >= result.edges.size()) continue;
    FlowLayoutEdge& edge = result.edges[it.key()];
    const FlowEdge* semantic = semantics.value(edge.id);
    if (!semantic) continue;
    QString curve = semantic->interpolate.isEmpty()
        ? options.curve : semantic->interpolate;
    if (curve.isEmpty() || curve == QLatin1String("basis"))
      curve = QStringLiteral("rounded");
    if (!curveSupportsLineHops(curve)) continue;
    edge.path = rewriteLineJumpPath(
        edge, *semantic, curve, it.value(),
        options.lineHops == QLatin1String("gap"));
  }
}

}  // namespace muffin::mermaid::flowchart

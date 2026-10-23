#include "mermaid/flowchart/SwimlaneOrthogonalRouter.h"

#include <QHash>
#include <QSet>
#include <QRectF>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace muffin::mermaid::flowchart::routing {
namespace {

constexpr qreal kEps = 1e-6;
constexpr qreal kRouterPadding = 8.0;
constexpr qreal kAnchorOffset = 20.0;
constexpr qreal kHorizontalPipeMargin = 15.0;
constexpr qreal kVerticalPipeMargin = 15.0;
constexpr qreal kRoutingMargin = 25.0;
constexpr qreal kCrossingPenalty = 1000.0;

QRectF nodeRect(const Node& node) {
  return QRectF(node.center.x() - node.size.width() / 2.0, node.center.y() - node.size.height() / 2.0,
                node.size.width(), node.size.height());
}

struct RouterSideInfo {
  int edgeIndex = -1;
  QString sourceId;
  QString targetId;
  OrthogonalSide sourceSide = OrthogonalSide::Bottom;
  OrthogonalSide targetSide = OrthogonalSide::Top;
  qreal absDx = 0.0;
  qreal absDy = 0.0;
  int dxSign = 0;
  int dySign = 0;
};

struct RouterObstacle {
  QString nodeId;
  qreal minX = 0.0;
  qreal minY = 0.0;
  qreal maxX = 0.0;
  qreal maxY = 0.0;
};

struct RouterPipe {
  bool vertical = false;
  qreal coord = 0.0;
  qreal spanMin = 0.0;
  qreal spanMax = 0.0;
};

struct RouterSegment {
  int edgeIndex = -1;
  bool vertical = false;
  qreal coord = 0.0;
  qreal from = 0.0;
  qreal to = 0.0;
};

struct RouterPortGroupEntry {
  int edgeIndex = -1;
  qreal oppositeCoord = 0.0;
};

int signOf(qreal value) {
  return value > 0.0 ? 1 : value < 0.0 ? -1 : 0;
}

QString sideName(OrthogonalSide side) {
  switch (side) {
  case OrthogonalSide::Top:
    return QStringLiteral("top");
  case OrthogonalSide::Bottom:
    return QStringLiteral("bottom");
  case OrthogonalSide::Left:
    return QStringLiteral("left");
  case OrthogonalSide::Right:
    return QStringLiteral("right");
  }
  return {};
}

bool sideIsVertical(OrthogonalSide side) {
  return side == OrthogonalSide::Top || side == OrthogonalSide::Bottom;
}

OrthogonalSide chooseOrthogonalSide(const Node& node, const QPointF& target, OrthogonalSide fallback) {
  const qreal dx = target.x() - node.center.x();
  const qreal dy = target.y() - node.center.y();
  const qreal absDx = std::abs(dx);
  const qreal absDy = std::abs(dy);
  if (absDx < kEps && absDy < kEps)
    return fallback;
  if (absDy > kEps && absDy * 3.0 >= absDx)
    return dy > 0.0 ? OrthogonalSide::Bottom : OrthogonalSide::Top;
  if (absDx > kEps)
    return dx > 0.0 ? OrthogonalSide::Right : OrthogonalSide::Left;
  return fallback;
}

QPointF portForSide(const Node& node, OrthogonalSide side) {
  const QRectF rect = nodeRect(node);
  switch (side) {
  case OrthogonalSide::Top:
    return QPointF(node.center.x(), rect.top());
  case OrthogonalSide::Bottom:
    return QPointF(node.center.x(), rect.bottom());
  case OrthogonalSide::Left:
    return QPointF(rect.left(), node.center.y());
  case OrthogonalSide::Right:
    return QPointF(rect.right(), node.center.y());
  }
  return node.center;
}

OrthogonalSide secondarySide(const RouterSideInfo& info) {
  if (sideIsVertical(info.sourceSide))
    return info.dxSign >= 0 ? OrthogonalSide::Right : OrthogonalSide::Left;
  return info.dySign >= 0 ? OrthogonalSide::Bottom : OrthogonalSide::Top;
}

qreal sidePreferenceStrength(const RouterSideInfo& info) {
  if (sideIsVertical(info.sourceSide))
    return info.absDx == 0.0 ? std::numeric_limits<qreal>::infinity() : info.absDy / info.absDx;
  return info.absDy == 0.0 ? std::numeric_limits<qreal>::infinity() : info.absDx / info.absDy;
}

QString sideLoadKey(const QString& nodeId, OrthogonalSide side) {
  return nodeId + QLatin1Char(':') + sideName(side);
}

QString portGroupKey(const QString& nodeId, OrthogonalSide side, bool sourceRole) {
  return sideLoadKey(nodeId, side) + (sourceRole ? QStringLiteral(":src") : QStringLiteral(":dst"));
}

QString portOffsetKey(int edgeIndex, bool sourceRole) {
  return QString::number(edgeIndex) + (sourceRole ? QStringLiteral(":src") : QStringLiteral(":dst"));
}

bool routerSegmentBlocked(const QPointF& first, const QPointF& second, const QVector<RouterObstacle>& obstacles,
                          const QString& sourceId, const QString& targetId) {
  const qreal minX = std::min(first.x(), second.x());
  const qreal maxX = std::max(first.x(), second.x());
  const qreal minY = std::min(first.y(), second.y());
  const qreal maxY = std::max(first.y(), second.y());
  for (const RouterObstacle& obstacle : obstacles) {
    if (obstacle.nodeId == sourceId || obstacle.nodeId == targetId)
      continue;
    if (std::abs(first.x() - second.x()) > kEps) {
      if (obstacle.minY < first.y() && obstacle.maxY > first.y() && obstacle.maxX > minX && obstacle.minX < maxX)
        return true;
    } else if (obstacle.minX < first.x() && obstacle.maxX > first.x() && obstacle.maxY > minY && obstacle.minY < maxY) {
      return true;
    }
  }
  return false;
}

QString routerPointKey(const QPointF& point) {
  return QString::number(point.x(), 'f', 1) + QLatin1Char(',') + QString::number(point.y(), 'f', 1);
}

// Mermaid 11.16 Phase 1: assign ports, construct visibility channels, route
// cross-lane edges first, then price crossings against the paths already placed.
class OrthogonalRouter {
public:
  OrthogonalRouter(const QVector<Node>& geometry, const QVector<FlowEdge>& sourceEdges)
      : orderedNodes(geometry), edges(sourceEdges), result(sourceEdges.size()) {
    for (const Node& node : geometry)
      nodes.insert(node.id, node);
  }

  QVector<QVector<QPointF>> run() {
    collectObstacles();
    chooseSides();
    balanceSides();
    distributePorts();
    for (int index : routingOrder())
      routeEdge(index);
    return std::move(result);
  }

private:
  const QVector<Node>& orderedNodes;
  const QVector<FlowEdge>& edges;
  QHash<QString, Node> nodes;
  QVector<RouterObstacle> obstacles;
  QVector<RouterSideInfo> sideInfos;
  QHash<int, int> sideInfoIndex;
  QHash<QString, int> incidentTotals;
  QHash<QString, QVector<RouterPortGroupEntry>> portGroups;
  QHash<QString, qreal> portOffsets;
  QVector<RouterPipe> pipes;
  QVector<RouterSegment> routedSegments;
  QVector<QVector<QPointF>> result;

  auto portsForEdge(int edgeIndex) const {
    const RouterSideInfo& info = sideInfos.at(sideInfoIndex.value(edgeIndex));
    QPointF source = portForSide(nodes.value(info.sourceId), info.sourceSide);
    QPointF target = portForSide(nodes.value(info.targetId), info.targetSide);
    if (portOffsets.contains(portOffsetKey(edgeIndex, true))) {
      const qreal offset = portOffsets.value(portOffsetKey(edgeIndex, true));
      sideIsVertical(info.sourceSide) ? source.rx() += offset : source.ry() += offset;
    }
    if (portOffsets.contains(portOffsetKey(edgeIndex, false))) {
      const qreal offset = portOffsets.value(portOffsetKey(edgeIndex, false));
      sideIsVertical(info.targetSide) ? target.rx() += offset : target.ry() += offset;
    }
    return std::tuple<QPointF, QPointF, OrthogonalSide, OrthogonalSide>(source, target, info.sourceSide,
                                                                        info.targetSide);
  }

  qreal getOrAddPipe(bool vertical, qreal coord, qreal spanMin, qreal spanMax) {
    for (RouterPipe& pipe : pipes) {
      if (pipe.vertical == vertical && std::abs(pipe.coord - coord) < 1.0) {
        pipe.spanMin = std::min(pipe.spanMin, spanMin);
        pipe.spanMax = std::max(pipe.spanMax, spanMax);
        return pipe.coord;
      }
    }
    pipes.push_back({vertical, coord, spanMin, spanMax});
    return coord;
  }

  qreal crossingCost(int edgeIndex, const QPointF& first, const QPointF& second) const {
    qreal cost = 0.0;
    const bool horizontal = std::abs(first.y() - second.y()) < kEps;
    const bool vertical = std::abs(first.x() - second.x()) < kEps;
    if (!horizontal && !vertical)
      return cost;
    for (const RouterSegment& segment : routedSegments) {
      if (segment.edgeIndex == edgeIndex || segment.vertical == vertical)
        continue;
      if (horizontal) {
        const qreal minX = std::min(first.x(), second.x()) - kEps;
        const qreal maxX = std::max(first.x(), second.x()) + kEps;
        if (segment.coord >= minX && segment.coord <= maxX && segment.from - kEps <= first.y() &&
            segment.to + kEps >= first.y())
          cost += kCrossingPenalty;
      } else {
        const qreal minY = std::min(first.y(), second.y()) - kEps;
        const qreal maxY = std::max(first.y(), second.y()) + kEps;
        if (segment.coord >= minY && segment.coord <= maxY && segment.from - kEps <= first.x() &&
            segment.to + kEps >= first.x())
          cost += kCrossingPenalty;
      }
    }
    return cost;
  }

  void collectObstacles() {
    for (const Node& node : orderedNodes) {
      if (!node.obstacle)
        continue;
      const QString& id = node.id;
      const QRectF rect = nodeRect(node);
      obstacles.push_back({id, rect.left() - kRouterPadding, rect.top() - kRouterPadding, rect.right() + kRouterPadding,
                           rect.bottom() + kRouterPadding});
    }
  }

  void chooseSides() {
    sideInfos.reserve(edges.size());
    for (int i = 0; i < edges.size(); ++i) {
      const FlowEdge& edge = edges.at(i);
      if (!nodes.contains(edge.start) || !nodes.contains(edge.end) || edge.start == edge.end)
        continue;
      ++incidentTotals[edge.start];
      ++incidentTotals[edge.end];
      const Node& source = nodes.value(edge.start);
      const Node& target = nodes.value(edge.end);
      const qreal dx = target.center.x() - source.center.x();
      const qreal dy = target.center.y() - source.center.y();
      RouterSideInfo info;
      info.edgeIndex = i;
      info.sourceId = edge.start;
      info.targetId = edge.end;
      info.sourceSide = chooseOrthogonalSide(source, target.center, OrthogonalSide::Bottom);
      info.targetSide = chooseOrthogonalSide(target, source.center, OrthogonalSide::Bottom);
      info.absDx = std::abs(dx);
      info.absDy = std::abs(dy);
      info.dxSign = signOf(dx);
      info.dySign = signOf(dy);
      sideInfoIndex.insert(i, sideInfos.size());
      sideInfos.push_back(info);
    }
  }

  void balanceSides() {
    QVector<QString> sourceGroupOrder;
    QHash<QString, QVector<int>> sourceGroups;
    QHash<QString, int> sideLoads;
    for (int i = 0; i < sideInfos.size(); ++i) {
      const RouterSideInfo& info = sideInfos.at(i);
      const QString groupKey = sideLoadKey(info.sourceId, info.sourceSide);
      if (!sourceGroups.contains(groupKey))
        sourceGroupOrder.push_back(groupKey);
      sourceGroups[groupKey].push_back(i);
      ++sideLoads[sideLoadKey(info.sourceId, info.sourceSide)];
      ++sideLoads[sideLoadKey(info.targetId, info.targetSide)];
    }
    for (const QString& key : sourceGroupOrder) {
      QVector<int>& group = sourceGroups[key];
      if (group.size() < 2)
        continue;
      std::stable_sort(group.begin(), group.end(), [&](int left, int right) {
        const qreal a = sidePreferenceStrength(sideInfos.at(left));
        const qreal b = sidePreferenceStrength(sideInfos.at(right));
        if (std::isfinite(a) != std::isfinite(b))
          return std::isfinite(b);
        if (std::isfinite(a) && std::abs(a - b) > 1e-9)
          return a > b;
        return sideInfos.at(left).edgeIndex < sideInfos.at(right).edgeIndex;
      });
      for (int position = 1; position < group.size(); ++position) {
        RouterSideInfo& info = sideInfos[group.at(position)];
        const OrthogonalSide secondary = secondarySide(info);
        const QString primaryKey = sideLoadKey(info.sourceId, info.sourceSide);
        const QString secondaryKey = sideLoadKey(info.sourceId, secondary);
        const int primaryLoad = sideLoads.value(primaryKey);
        const int secondaryLoad = sideLoads.value(secondaryKey);
        if (secondaryLoad >= primaryLoad)
          continue;
        sideLoads[primaryKey] = primaryLoad - 1;
        sideLoads[secondaryKey] = secondaryLoad + 1;
        info.sourceSide = secondary;
      }
    }
  }

  void distributePorts() {
    QVector<QString> portGroupOrder;
    const auto appendPortGroup = [&](const QString& key, RouterPortGroupEntry entry) {
      if (!portGroups.contains(key))
        portGroupOrder.push_back(key);
      portGroups[key].push_back(entry);
    };
    for (const RouterSideInfo& info : sideInfos) {
      const Node& source = nodes.value(info.sourceId);
      const Node& target = nodes.value(info.targetId);
      appendPortGroup(portGroupKey(info.sourceId, info.sourceSide, true),
                      {info.edgeIndex, sideIsVertical(info.sourceSide) ? target.center.x() : target.center.y()});
      appendPortGroup(portGroupKey(info.targetId, info.targetSide, false),
                      {info.edgeIndex, sideIsVertical(info.targetSide) ? source.center.x() : source.center.y()});
    }
    for (const QString& key : portGroupOrder) {
      QVector<RouterPortGroupEntry>& group = portGroups[key];
      if (group.size() < 2)
        continue;
      std::stable_sort(group.begin(), group.end(),
                       [](const auto& left, const auto& right) { return left.oppositeCoord < right.oppositeCoord; });
      const QStringList parts = key.split(QLatin1Char(':'));
      if (parts.size() < 3)
        continue;
      const QString role = parts.last();
      const QString sideText = parts.at(parts.size() - 2);
      const QString nodeId = parts.mid(0, parts.size() - 2).join(QLatin1Char(':'));
      if (!nodes.contains(nodeId))
        continue;
      const Node& node = nodes.value(nodeId);
      const bool verticalSide = sideText == QLatin1String("left") || sideText == QLatin1String("right");
      const qreal sideLength = verticalSide ? node.size.height() : node.size.width();
      const qreal spacing = std::min<qreal>(20.0, std::max<qreal>(8.0, sideLength / (group.size() + 1.0)));
      const qreal firstOffset = -spacing * (group.size() - 1) / 2.0;
      for (int i = 0; i < group.size(); ++i)
        portOffsets.insert(portOffsetKey(group.at(i).edgeIndex, role == QLatin1String("src")),
                           firstOffset + i * spacing);
    }
  }

  QVector<int> routingOrder() const {
    QVector<int> routingOrder;
    routingOrder.reserve(edges.size());
    for (int i = 0; i < edges.size(); ++i)
      if (sideInfoIndex.contains(i))
        routingOrder.push_back(i);
    std::stable_sort(routingOrder.begin(), routingOrder.end(), [&](int left, int right) {
      const FlowEdge& a = edges.at(left);
      const FlowEdge& b = edges.at(right);
      const bool aCross = nodes.value(a.start).lane != nodes.value(a.end).lane;
      const bool bCross = nodes.value(b.start).lane != nodes.value(b.end).lane;
      if (aCross != bCross)
        return aCross;
      const Node& as = nodes.value(a.start);
      const Node& at = nodes.value(a.end);
      const Node& bs = nodes.value(b.start);
      const Node& bt = nodes.value(b.end);
      const qreal aDistance = std::abs(at.center.x() - as.center.x()) + std::abs(at.center.y() - as.center.y());
      const qreal bDistance = std::abs(bt.center.x() - bs.center.x()) + std::abs(bt.center.y() - bs.center.y());
      if (std::abs(aDistance - bDistance) > 1.0)
        return aDistance < bDistance;
      return left < right;
    });

    return routingOrder;
  }

  QVector<QPointF> findGridPath(int edgeIndex, const QPointF& sourceAnchor, const QPointF& targetAnchor, qreal minX,
                                qreal maxX, qreal minY, qreal maxY) const {
    const auto& edge = edges.at(edgeIndex);
    QVector<QPointF> path;

    QVector<qreal> horizontalPipes;
    QVector<qreal> verticalPipes;
    for (const RouterPipe& pipe : pipes) {
      if (pipe.vertical && pipe.coord >= minX && pipe.coord <= maxX)
        verticalPipes.push_back(pipe.coord);
      else if (!pipe.vertical && pipe.coord >= minY && pipe.coord <= maxY)
        horizontalPipes.push_back(pipe.coord);
    }
    std::sort(horizontalPipes.begin(), horizontalPipes.end());
    std::sort(verticalPipes.begin(), verticalPipes.end());

    struct OpenPoint {
      QString key;
      qreal score = 0.0;
      QPointF point;
    };
    QVector<OpenPoint> open{{routerPointKey(sourceAnchor),
                             std::hypot(targetAnchor.x() - sourceAnchor.x(), targetAnchor.y() - sourceAnchor.y()),
                             sourceAnchor}};
    QSet<QString> openKeys{routerPointKey(sourceAnchor)};
    QHash<QString, qreal> scores{{routerPointKey(sourceAnchor), 0.0}};
    QHash<QString, QPointF> previous;
    QHash<QString, QChar> arrival{{routerPointKey(sourceAnchor), QLatin1Char('n')}};
    while (!open.isEmpty()) {
      std::stable_sort(open.begin(), open.end(),
                       [](const OpenPoint& a, const OpenPoint& b) { return a.score < b.score; });
      const OpenPoint current = open.takeFirst();
      openKeys.remove(current.key);
      if (current.key == routerPointKey(targetAnchor)) {
        QPointF cursor = targetAnchor;
        QString key = current.key;
        path.prepend(cursor);
        while (previous.contains(key)) {
          cursor = previous.value(key);
          path.prepend(cursor);
          key = routerPointKey(cursor);
        }
        break;
      }
      const auto adjacent = [](const QVector<qreal>& values, qreal value) {
        QVector<qreal> result;
        int index = -1;
        for (int i = 0; i < values.size(); ++i)
          if (std::abs(values.at(i) - value) < 1.0) {
            index = i;
            break;
          }
        if (index > 0)
          result.push_back(values.at(index - 1));
        if (index >= 0 && index + 1 < values.size())
          result.push_back(values.at(index + 1));
        return result;
      };
      QVector<QPointF> neighbors;
      for (qreal x : adjacent(verticalPipes, current.point.x()))
        neighbors.push_back(QPointF(x, current.point.y()));
      for (qreal y : adjacent(horizontalPipes, current.point.y()))
        neighbors.push_back(QPointF(current.point.x(), y));
      for (const QPointF& neighbor : neighbors) {
        if (routerSegmentBlocked(current.point, neighbor, obstacles, edge.start, edge.end))
          continue;
        const qreal moveX = neighbor.x() - current.point.x();
        const qreal moveY = neighbor.y() - current.point.y();
        qreal directionalPenalty = 0.0;
        const qreal destinationX = targetAnchor.x() - sourceAnchor.x();
        const qreal destinationY = targetAnchor.y() - sourceAnchor.y();
        if ((destinationY > 10.0 && moveY < -5.0) || (destinationY < -10.0 && moveY > 5.0))
          directionalPenalty = std::abs(moveY) * 100.0;
        if ((destinationX > 10.0 && moveX < -5.0) || (destinationX < -10.0 && moveX > 5.0))
          directionalPenalty += std::abs(moveX) * 50.0;
        const QChar moveDirection = std::abs(moveX) > kEps ? QLatin1Char('h') : QLatin1Char('v');
        const QChar currentDirection = arrival.value(current.key, QLatin1Char('n'));
        const qreal bendPenalty =
            currentDirection != QLatin1Char('n') && currentDirection != moveDirection ? 50.0 : 0.0;
        const qreal tentative = scores.value(current.key, std::numeric_limits<qreal>::infinity()) + std::abs(moveX) +
                                std::abs(moveY) + directionalPenalty + bendPenalty +
                                crossingCost(edgeIndex, current.point, neighbor);
        const QString neighborKey = routerPointKey(neighbor);
        if (tentative >= scores.value(neighborKey, std::numeric_limits<qreal>::infinity()))
          continue;
        previous.insert(neighborKey, current.point);
        scores.insert(neighborKey, tentative);
        arrival.insert(neighborKey, moveDirection);
        const qreal heuristic = std::abs(targetAnchor.x() - neighbor.x()) + std::abs(targetAnchor.y() - neighbor.y());
        bool found = false;
        for (OpenPoint& item : open) {
          if (item.key != neighborKey)
            continue;
          item.score = tentative + heuristic;
          found = true;
          break;
        }
        if (!found) {
          open.push_back({neighborKey, tentative + heuristic, neighbor});
          openKeys.insert(neighborKey);
        }
      }
    }

    return path;
  }

  void routeEdge(int edgeIndex) {
    const FlowEdge& edge = edges.at(edgeIndex);
    auto [sourcePort, targetPort, sourceSide, targetSide] = portsForEdge(edgeIndex);
    QPointF sourceAnchor = sourcePort;
    QPointF targetAnchor = targetPort;
    if (sideIsVertical(sourceSide))
      sourceAnchor.ry() += sourceSide == OrthogonalSide::Bottom ? kAnchorOffset : -kAnchorOffset;
    else
      sourceAnchor.rx() += sourceSide == OrthogonalSide::Right ? kAnchorOffset : -kAnchorOffset;
    if (sideIsVertical(targetSide))
      targetAnchor.ry() += targetSide == OrthogonalSide::Bottom ? kAnchorOffset : -kAnchorOffset;
    else
      targetAnchor.rx() += targetSide == OrthogonalSide::Right ? kAnchorOffset : -kAnchorOffset;

    const bool anchorsSameX = std::abs(sourceAnchor.x() - targetAnchor.x()) < kHorizontalPipeMargin;
    const bool anchorsSameY = std::abs(sourceAnchor.y() - targetAnchor.y()) < kHorizontalPipeMargin;
    const bool hasPortOffset =
        portOffsets.contains(portOffsetKey(edgeIndex, true)) || portOffsets.contains(portOffsetKey(edgeIndex, false));
    const int sourceFaceTotal = portGroups.value(portGroupKey(edge.start, sourceSide, true)).size() +
                                portGroups.value(portGroupKey(edge.start, sourceSide, false)).size();
    const int targetFaceTotal = portGroups.value(portGroupKey(edge.end, targetSide, true)).size() +
                                portGroups.value(portGroupKey(edge.end, targetSide, false)).size();
    const bool faceContested = sourceFaceTotal > 1 || targetFaceTotal > 1;
    const bool simpleContested = faceContested && edge.text.isEmpty() &&
                                 (sourceFaceTotal <= 1 || incidentTotals.value(edge.start) <= 2) &&
                                 (targetFaceTotal <= 1 || incidentTotals.value(edge.end) <= 2);
    if ((anchorsSameX || anchorsSameY) && !hasPortOffset && (!faceContested || simpleContested) &&
        !routerSegmentBlocked(sourcePort, targetPort, obstacles, edge.start, edge.end)) {
      result[edgeIndex] = {sourcePort, targetPort};
      const bool vertical = anchorsSameX && !anchorsSameY;
      routedSegments.push_back(
          {edgeIndex, vertical, vertical ? sourcePort.x() : sourcePort.y(),
           vertical ? std::min(sourcePort.y(), targetPort.y()) : std::min(sourcePort.x(), targetPort.x()),
           vertical ? std::max(sourcePort.y(), targetPort.y()) : std::max(sourcePort.x(), targetPort.x())});
      return;
    }

    sourceAnchor.setX(getOrAddPipe(true, sourceAnchor.x(), sourceAnchor.y(), sourceAnchor.y()));
    targetAnchor.setX(getOrAddPipe(true, targetAnchor.x(), targetAnchor.y(), targetAnchor.y()));
    qreal minX = std::min(sourceAnchor.x(), targetAnchor.x()) - 50.0;
    qreal maxX = std::max(sourceAnchor.x(), targetAnchor.x()) + 50.0;
    qreal minY = std::min(sourceAnchor.y(), targetAnchor.y()) - 50.0;
    qreal maxY = std::max(sourceAnchor.y(), targetAnchor.y()) + 50.0;
    for (const RouterObstacle& obstacle : obstacles) {
      const bool blocksCorridor = obstacle.minX < std::max(sourceAnchor.x(), targetAnchor.x()) &&
                                  obstacle.maxX > std::min(sourceAnchor.x(), targetAnchor.x()) &&
                                  obstacle.minY < std::max(sourceAnchor.y(), targetAnchor.y()) &&
                                  obstacle.maxY > std::min(sourceAnchor.y(), targetAnchor.y());
      if (!blocksCorridor)
        continue;
      minX = std::min(minX, obstacle.minX - kRoutingMargin);
      maxX = std::max(maxX, obstacle.maxX + kRoutingMargin);
      minY = std::min(minY, obstacle.minY - kRoutingMargin);
      maxY = std::max(maxY, obstacle.maxY + kRoutingMargin);
    }
    for (const RouterObstacle& obstacle : obstacles) {
      if (obstacle.maxX < minX || obstacle.minX > maxX || obstacle.maxY < minY || obstacle.minY > maxY)
        continue;
      getOrAddPipe(false, obstacle.minY - kHorizontalPipeMargin, minX, maxX);
      getOrAddPipe(false, obstacle.maxY + kHorizontalPipeMargin, minX, maxX);
      getOrAddPipe(true, obstacle.minX - kVerticalPipeMargin, minY, maxY);
      getOrAddPipe(true, obstacle.maxX + kVerticalPipeMargin, minY, maxY);
    }
    getOrAddPipe(false, sourceAnchor.y(), minX, maxX);
    getOrAddPipe(false, targetAnchor.y(), minX, maxX);

    QVector<QPointF> path;
    const QPointF horizontalCorner(targetAnchor.x(), sourceAnchor.y());
    const QPointF verticalCorner(sourceAnchor.x(), targetAnchor.y());
    const bool horizontalFirstBlocked =
        routerSegmentBlocked(sourceAnchor, horizontalCorner, obstacles, edge.start, edge.end) ||
        routerSegmentBlocked(horizontalCorner, targetAnchor, obstacles, edge.start, edge.end);
    const bool verticalFirstBlocked =
        routerSegmentBlocked(sourceAnchor, verticalCorner, obstacles, edge.start, edge.end) ||
        routerSegmentBlocked(verticalCorner, targetAnchor, obstacles, edge.start, edge.end);
    if (!horizontalFirstBlocked) {
      path =
          (std::abs(sourceAnchor.y() - targetAnchor.y()) < kEps || std::abs(sourceAnchor.x() - targetAnchor.x()) < kEps)
              ? QVector<QPointF>{sourceAnchor, targetAnchor}
              : QVector<QPointF>{sourceAnchor, horizontalCorner, targetAnchor};
    } else if (!verticalFirstBlocked) {
      path = std::abs(sourceAnchor.x() - targetAnchor.x()) < kEps
                 ? QVector<QPointF>{sourceAnchor, targetAnchor}
                 : QVector<QPointF>{sourceAnchor, verticalCorner, targetAnchor};
    }

    if (path.isEmpty())
      path = findGridPath(edgeIndex, sourceAnchor, targetAnchor, minX, maxX, minY, maxY);
    if (path.isEmpty())
      path = {sourceAnchor, QPointF(sourceAnchor.x(), targetAnchor.y()), targetAnchor};
    QVector<QPointF> full{sourcePort};
    full += path;
    full.push_back(targetPort);
    result[edgeIndex] = simplifyRouterPoints(full);
    const QVector<QPointF>& routed = result.at(edgeIndex);
    for (int i = 0; i + 1 < routed.size(); ++i) {
      const QPointF& first = routed.at(i);
      const QPointF& second = routed.at(i + 1);
      const bool vertical = std::abs(first.x() - second.x()) < kEps;
      routedSegments.push_back({edgeIndex, vertical, vertical ? first.x() : first.y(),
                                vertical ? std::min(first.y(), second.y()) : std::min(first.x(), second.x()),
                                vertical ? std::max(first.y(), second.y()) : std::max(first.x(), second.x())});
    }
  }
};

} // namespace

QVector<QPointF> simplifyRouterPoints(QVector<QPointF> points) {
  if (points.size() < 2)
    return points;
  QVector<QPointF> deduped;
  for (const QPointF& point : points) {
    if (deduped.isEmpty() || std::abs(deduped.last().x() - point.x()) > kEps ||
        std::abs(deduped.last().y() - point.y()) > kEps)
      deduped.push_back(point);
  }
  QVector<QPointF> result;
  for (const QPointF& point : deduped) {
    while (result.size() >= 2) {
      const QPointF& a = result.at(result.size() - 2);
      const QPointF& b = result.last();
      const bool horizontal = std::abs(a.y() - b.y()) < kEps && std::abs(b.y() - point.y()) < kEps;
      const bool vertical = std::abs(a.x() - b.x()) < kEps && std::abs(b.x() - point.x()) < kEps;
      if (!horizontal && !vertical)
        break;
      const qreal firstDelta = horizontal ? b.x() - a.x() : b.y() - a.y();
      const qreal nextDelta = horizontal ? point.x() - b.x() : point.y() - b.y();
      if (signOf(firstDelta) != signOf(nextDelta))
        break;
      result.removeLast();
    }
    result.push_back(point);
  }
  return result;
}

QVector<QVector<QPointF>> routeOrthogonalEdges(const QVector<Node>& nodes, const QVector<FlowEdge>& edges) {
  return OrthogonalRouter(nodes, edges).run();
}

} // namespace muffin::mermaid::flowchart::routing

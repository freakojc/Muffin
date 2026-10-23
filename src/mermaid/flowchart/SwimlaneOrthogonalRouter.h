#pragma once

#include "mermaid/flowchart/Flowchart.h"
#include <QPointF>
#include <QSizeF>

namespace muffin::mermaid::flowchart::routing {

enum class OrthogonalSide { Top, Bottom, Left, Right };

// A frozen geometry snapshot. Routing does not know or mutate the ranking
// graph; the caller supplies the top-level lane used to prioritize crossings.
struct Node {
  QString id;
  QString lane;
  QPointF center;
  QSizeF size;
  bool obstacle = true;
};

QVector<QVector<QPointF>> routeOrthogonalEdges(const QVector<Node>& nodes, const QVector<FlowEdge>& edges);
QVector<QPointF> simplifyRouterPoints(QVector<QPointF> points);

} // namespace muffin::mermaid::flowchart::routing

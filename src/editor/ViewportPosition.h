#pragma once

#include <QtTypes>

namespace muffin {

// Persistent reading position: UTF-16 source anchor and its screen-space Y.
// Node ids and absolute scroll values belong to one layout and cannot survive
// a new parse, viewport width, or theme. Text anchors also survive rewrapping.
struct ViewportPosition {
  qsizetype sourceOffset = -1;
  qreal screenOffset = 0;
  int horizontal = 0;
  bool textAnchor = false;
  bool atStart = false;
};

}  // namespace muffin

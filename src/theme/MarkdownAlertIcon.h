#pragma once

#include "document/MarkdownTypes.h"
#include <QByteArray>

namespace muffin {

// Small, theme-independent semantic icons. Colour and geometry come from the
// live SVG element's computed style, including the alert's inherited colour.
inline QByteArray markdownAlertIconPath(AlertKind kind) {
  switch (kind) {
  case AlertKind::Note:
    return "M8 1a7 7 0 1 0 0 14A7 7 0 0 0 8 1Zm0 1.5a5.5 5.5 0 1 1 0 11 5.5 5.5 0 0 1 0-11ZM7.25 7h1.5v5h-1.5ZM8 4a1 1 0 1 0 0 2 1 1 0 0 0 0-2Z";
  case AlertKind::Tip:
    return "M8 0a5 5 0 0 0-3 9v3h6V9a5 5 0 0 0-3-9Zm0 1.5a3.5 3.5 0 0 1 2.1 6.3l-.6.45v2.25h-3V8.25l-.6-.45A3.5 3.5 0 0 1 8 1.5ZM5 13h6v1.5H5Zm1.5 2h3v1h-3Z";
  case AlertKind::Important:
    return "M2 1h12a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H6l-4 3V2a1 1 0 0 1 0-1Zm1.5 1.5V12l2-1.5h8V2.5ZM7.25 4h1.5v4h-1.5ZM8 8.75a.875.875 0 1 0 0 1.75.875.875 0 0 0 0-1.75Z";
  case AlertKind::Warning:
    return "M7.13 1.5a1 1 0 0 1 1.74 0l6.93 12a1 1 0 0 1-.87 1.5H1.07a1 1 0 0 1-.87-1.5ZM8 3 1.94 13.5h12.12ZM7.25 6h1.5v4h-1.5ZM8 10.75a.875.875 0 1 0 0 1.75.875.875 0 0 0 0-1.75Z";
  case AlertKind::Caution:
    return "M4.5 0h7L16 4.5v7L11.5 16h-7L0 11.5v-7Zm.62 1.5L1.5 5.12v5.76l3.62 3.62h5.76l3.62-3.62V5.12L10.88 1.5ZM7.25 4h1.5v5h-1.5ZM8 10a1 1 0 1 0 0 2 1 1 0 0 0 0-2Z";
  default:
    return {};
  }
}

inline QByteArray markdownAlertIconSvg(AlertKind kind) {
  return "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16' width='16' height='16'><path d='" +
         markdownAlertIconPath(kind) + "'/></svg>";
}

} // namespace muffin

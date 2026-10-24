#pragma once

#include <QString>

namespace muffin {

class HtmlSanitizer final {
public:
  QString sanitizedPreview(QString html) const;
  // The same URL/active-content policy as preview, retaining the HTML hidden
  // attribute so clipboard projection can distinguish content from scaffolding.
  QString sanitizedClipboard(QString html) const;
  QString sanitizedMermaidText(QString html) const;
};

} // namespace muffin

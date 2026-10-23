#pragma once

#include <QString>
#include <QStringList>

class QFont;

namespace muffin::mermaid {

class MermaidFontRegistry {
public:
  static void ensureLoaded();
  static QString primaryFamily();
  static QString cssFamilyStack();
  static QStringList familyStack();
  static QString bundledFamily(const QString& declaredFamily);
  // Resolve bundled names while preserving other families and their CSS order.
  static QStringList resolveFamilies(QStringList families);

  // Expands the fixed CSS stack into Qt's ordered fallback family list.
  static void configureFont(QFont& font, const QString& familyExpression);
};

} // namespace muffin::mermaid

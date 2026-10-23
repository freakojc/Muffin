#pragma once

#include "theme/CssThemeParser.h"

#include <QJsonObject>

namespace muffin {

// An explicit, offline stylesheet audit, not a prediction of the final cascade
// or pixel parity. Never run this while discovering a theme directory. Stable
// machine-readable codes/messages intentionally do not depend on UI language.
class CssCompatibilityReport {
public:
  static CssCompatibilityReport analyze(const CssThemeSheet& sheet, const QString& baseDir = {},
                                        const CssEnvironment& environment = {});
  QJsonObject toJson() const { return report_; }

private:
  QJsonObject report_;
};

} // namespace muffin

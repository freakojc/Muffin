#pragma once

#include "theme/ThemeDefinition.h"

#include <QString>
#include <QVector>

#include <functional>
#include <optional>
#include <vector>

namespace muffin {

// Format a 1-based counter value per a CSS list-style-type / counter() style
// (decimal, decimal-leading-zero, lower/upper-alpha & -latin, lower/upper-roman,
// lower-greek). Returns the raw value (no trailing punctuation).
QString formatCounterValue(int value, const QString& style);

// A counter declaration is atomic: an invalid name/value rejects the whole list.
std::optional<QVector<QPair<QString, int>>> parseCounterOperations(const QString& text, int defaultValue);

// Split a CSS `content` value into literal / counter() / counters() / string attr() tokens.
// `counter(name)` / `counter(name, style)` / `counters(name, "sep")` /
// `counters(name, "sep", style)` and `attr(name[, "fallback"])` are recognized.
// Numeric/unit-typed attr() is unsupported. CSS strings decode their escapes.
std::vector<ContentToken> parseContentTokens(const QString& content);

// Resolve a token list to a string. `value(name)` returns the counter's current
// value; `chain(name)` returns its ancestor-chain values (outermost first) for
// `counters(name, sep)` (which joins them with the separator). Literal tokens are
// appended verbatim. `attribute(name)` distinguishes absent from present-empty.
QString resolveContentTokens(const std::vector<ContentToken>& tokens,
                             const std::function<int(const QString&)>& value,
                             const std::function<QVector<int>(const QString&)>& chain,
                             const std::function<std::optional<QString>(const QString&)>& attribute = {});

}  // namespace muffin

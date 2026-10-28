#pragma once

#include <QString>
#include <array>
#include <optional>

namespace muffin {

// 表格「勾选列」使用的字形对：选中 = 实心，未选中 = 空心，成对出现，
// 点击单元格时在两者之间切换。可选彩色 emoji 的优先用彩色 emoji。
struct ChecklistGlyphPair {
  QString checked;   // 实心
  QString unchecked; // 空心
};

inline const std::array<ChecklistGlyphPair, 4>& checklistGlyphPairs() {
  static const std::array<ChecklistGlyphPair, 4> pairs{{
      {QStringLiteral("⭐"), QStringLiteral("☆")}, // 星标
      {QStringLiteral("☑️"), QStringLiteral("☐")},  // 勾状
      {QStringLiteral("■"), QStringLiteral("□")},  // 方形（黑/白）
      {QStringLiteral("●"), QStringLiteral("○")},  // 圆点（黑/白）
  }};
  return pairs;
}

inline int checklistGlyphPairCount() {
  return static_cast<int>(checklistGlyphPairs().size());
}

// 去掉变体选择符（U+FE0F）后比较，让手打的 ☑ 与 emoji 版 ☑️ 都能命中同一对。
inline QString checklistNormalized(QString text) {
  text.remove(QChar(0xFE0F));
  return text.trimmed();
}

// 单元格内容（去首尾空白）恰好等于某个字形时，返回它所属的那一对。
inline std::optional<ChecklistGlyphPair> checklistPairForCellText(const QString& text) {
  const QString normalized = checklistNormalized(text);
  if (normalized.isEmpty()) {
    return std::nullopt;
  }
  for (const auto& pair : checklistGlyphPairs()) {
    if (normalized == checklistNormalized(pair.checked) || normalized == checklistNormalized(pair.unchecked)) {
      return pair;
    }
  }
  return std::nullopt;
}

// 返回与当前内容相反的那一端（实心 ↔ 空心），用对里的规范字形。
inline QString checklistOppositeGlyph(const ChecklistGlyphPair& pair, const QString& current) {
  return checklistNormalized(current) == checklistNormalized(pair.checked) ? pair.unchecked : pair.checked;
}

} // namespace muffin
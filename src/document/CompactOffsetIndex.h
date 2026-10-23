#pragma once

#include <QVector>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace muffin {

// Sorted, nonnegative UTF-16 offsets. Ordinary documents use four bytes per
// entry; promotion preserves qsizetype's complete domain. Qt buffers remain
// implicitly shared when a PieceTable is copied for background preparation.
class CompactOffsetIndex {
public:
  void push_back(qsizetype offset) {
    Q_ASSERT(offset >= 0);
    if (auto* narrow = std::get_if<Narrow>(&values_)) {
      if (quint64(offset) <= std::numeric_limits<quint32>::max()) {
        narrow->push_back(quint32(offset));
        return;
      }
      Wide wide;
      wide.reserve(narrow->size() + 1);
      for (quint32 value : std::as_const(*narrow))
        wide.push_back(qsizetype(value));
      values_ = std::move(wide);
    }
    std::get<Wide>(values_).push_back(offset);
  }

  qsizetype size() const {
    return std::visit([](const auto& values) { return values.size(); }, values_);
  }
  qsizetype at(qsizetype index) const {
    return std::visit([index](const auto& values) { return qsizetype(values.at(index)); }, values_);
  }
  qsizetype lowerBound(qsizetype offset, qsizetype from = 0) const {
    if (offset < 0)
      return from;
    return std::visit(
        [=](const auto& values) {
          return std::lower_bound(values.cbegin() + from, values.cend(), offset) - values.cbegin();
        },
        values_);
  }
  const void* data() const {
    return std::visit([](const auto& values) -> const void* { return values.constData(); }, values_);
  }
  qint64 allocatedBytes() const {
    return std::visit(
        [](const auto& values) {
          return qint64(values.capacity()) * sizeof(typename std::decay_t<decltype(values)>::value_type);
        },
        values_);
  }

private:
  using Narrow = QVector<quint32>;
  using Wide = QVector<qsizetype>;
  std::variant<Narrow, Wide> values_;
};

} // namespace muffin

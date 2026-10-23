#include "theme/ThemeFontDecoder.h"
#include "theme/ThemeResourceDigest.h"
#include "diagnostics/ScopedPerfProbe.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include <QCryptographicHash>
#include <QFontDatabase>
#include <QHash>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <tuple>
#include <vector>

namespace muffin {
namespace {
ThemeFontRegistrationStats registrationStats;
Q_LOGGING_CATEGORY(fontLoadPerf, "muffin.perf.detail", QtWarningMsg)
constexpr qsizetype maxFontBytes = 64 * 1024 * 1024;
void put16(QByteArray& bytes, qsizetype offset, quint16 value) {
  qToBigEndian(value, bytes.data() + offset);
}
void put32(QByteArray& bytes, qsizetype offset, quint32 value) {
  qToBigEndian(value, bytes.data() + offset);
}
quint32 checksum(const QByteArray& bytes) {
  diag::ScopedPerfProbe perf("font.checksum", fontLoadPerf());
  quint32 sum = 0;
  qsizetype i = 0;
  for (; i + 4 <= bytes.size(); i += 4)
    sum += qFromBigEndian<quint32>(bytes.constData() + i);
  if (i < bytes.size()) {
    quint32 tail = 0;
    for (int j = 0; j < 4; ++j)
      tail = (tail << 8) | (i + j < bytes.size() ? quint8(bytes[i + j]) : 0);
    sum += tail;
  }
  return sum;
}
} // namespace
QByteArray decodeThemeWebFont(const QByteArray& source) {
  diag::ScopedPerfProbe perf("font.decode", fontLoadPerf());
  if (source.size() < 4 || source.size() > maxFontBytes)
    return {};
  if (!source.startsWith("wOFF") && !source.startsWith("wOF2"))
    return source;
  FT_Library library = nullptr;
  if (FT_Init_FreeType(&library))
    return {};
  FT_Face face = nullptr;
  if (FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(source.constData()),
                         static_cast<FT_Long>(source.size()), 0, &face)) {
    FT_Done_FreeType(library);
    return {};
  }
  struct Table {
    quint32 tag;
    QByteArray bytes;
  };
  std::vector<Table> tables;
  FT_ULong count = 0;
  const bool valid = FT_Sfnt_Table_Info(face, 0, nullptr, &count) == 0 && count > 0 && count <= 256;
  qsizetype total = 12 + 16 * count;
  if (valid)
    for (FT_ULong i = 0; i < count; ++i) {
      FT_ULong tag = 0, length = 0;
      if (FT_Sfnt_Table_Info(face, i, &tag, &length) || length > maxFontBytes || total + length + 3 > maxFontBytes) {
        tables.clear();
        break;
      }
      QByteArray bytes(static_cast<qsizetype>(length), Qt::Uninitialized);
      if (FT_Load_Sfnt_Table(face, tag, 0, reinterpret_cast<FT_Byte*>(bytes.data()), &length)) {
        tables.clear();
        break;
      }
      if (tag == FT_MAKE_TAG('h', 'e', 'a', 'd') && bytes.size() >= 12)
        put32(bytes, 8, 0);
      total += (length + 3) & ~FT_ULong(3);
      tables.push_back({static_cast<quint32>(tag), std::move(bytes)});
    }
  FT_Done_Face(face);
  FT_Done_FreeType(library);
  if (tables.size() != count || tables.empty())
    return {};
  std::sort(tables.begin(), tables.end(), [](const auto& a, const auto& b) { return a.tag < b.tag; });
  QByteArray result(total, '\0');
  const bool cff = std::any_of(tables.begin(), tables.end(), [](const auto& t) {
    return t.tag == FT_MAKE_TAG('C', 'F', 'F', ' ') || t.tag == FT_MAKE_TAG('C', 'F', 'F', '2');
  });
  put32(result, 0, cff ? FT_MAKE_TAG('O', 'T', 'T', 'O') : 0x00010000);
  put16(result, 4, static_cast<quint16>(count));
  quint16 power = 1, selector = 0;
  while (power * 2 <= count) {
    power *= 2;
    ++selector;
  }
  put16(result, 6, power * 16);
  put16(result, 8, selector);
  put16(result, 10, count * 16 - power * 16);
  qsizetype offset = 12 + 16 * count, headOffset = -1;
  for (std::size_t i = 0; i < tables.size(); ++i) {
    const auto& table = tables[i];
    const qsizetype entry = 12 + 16 * i;
    put32(result, entry, table.tag);
    put32(result, entry + 4, checksum(table.bytes));
    put32(result, entry + 8, static_cast<quint32>(offset));
    put32(result, entry + 12, static_cast<quint32>(table.bytes.size()));
    std::copy(table.bytes.begin(), table.bytes.end(), result.begin() + offset);
    if (table.tag == FT_MAKE_TAG('h', 'e', 'a', 'd'))
      headOffset = offset;
    offset += (table.bytes.size() + 3) & ~qsizetype(3);
  }
  if (headOffset < 0 || headOffset + 12 > result.size())
    return {};
  put32(result, headOffset + 8, 0xB1B0AFBA - checksum(result));
  return result;
}
namespace {
QByteArray transformThemeFont(const QByteArray& native, const QString& family, std::optional<int> weight,
                              std::optional<bool> italic, bool validateOnly) {
  diag::ScopedPerfProbe perf("font.isolate", fontLoadPerf());
  if (native.size() < 12 || native.size() > maxFontBytes || family.isEmpty())
    return {};
  const auto get16 = [](const QByteArray& b, qsizetype at) { return qFromBigEndian<quint16>(b.constData() + at); };
  const auto get32 = [](const QByteArray& b, qsizetype at) { return qFromBigEndian<quint32>(b.constData() + at); };
  const auto signature = get32(native, 0);
  if (signature != 0x00010000 && signature != 0x4f54544f)
    return {}; // SFNT; collections require face selection.
  const auto count = get16(native, 4);
  if (!count || count > 256 || 12 + 16 * count > native.size())
    return {};
  struct Table {
    quint32 tag;
    QByteArray bytes;
  };
  std::vector<Table> tables;
  bool hasHead = false, hasOs2 = false;
  for (int i = 0; i < count; ++i) {
    const auto entry = 12 + 16 * i;
    const auto offset = get32(native, entry + 8), length = get32(native, entry + 12);
    if (quint64(offset) + length > quint64(native.size()))
      return {};
    const auto tag = get32(native, entry);
    if (std::any_of(tables.begin(), tables.end(), [&](const auto& table) { return table.tag == tag; }))
      return {};
    if (tag == 0x68656164) {
      if (length < 54)
        return {};
      hasHead = true;
    }
    if (tag == 0x4f532f32) {
      if (length < 64)
        return {};
      hasOs2 = true;
    }
    // Untouched glyph tables are views during this call. Mutating name/head/
    // OS2 detaches their tiny buffers; the returned font owns its full output.
    tables.push_back({tag, QByteArray::fromRawData(native.constData() + offset, length)});
  }
  if (!hasHead || ((weight || italic) && !hasOs2))
    return {};
  int actualWeight = 400;
  bool actualItalic = false;
  for (const auto& table : tables)
    if (table.tag == 0x4f532f32 && table.bytes.size() >= 64) {
      actualWeight = get16(table.bytes, 4);
      actualItalic = get16(table.bytes, 62) & 1;
    }
  actualWeight = weight.value_or(actualWeight);
  actualItalic = italic.value_or(actualItalic);
  static const QHash<int, QString> weightNames{{100, "Thin"},    {200, "ExtraLight"}, {300, "Light"},
                                               {400, "Regular"}, {500, "Medium"},     {600, "SemiBold"},
                                               {700, "Bold"},    {800, "ExtraBold"},  {900, "Black"}};
  auto subfamily = weightNames.value(actualWeight, "Weight" + QString::number(actualWeight));
  if (actualItalic)
    subfamily = actualWeight == 400 ? QStringLiteral("Italic") : subfamily + " Italic";
  struct Name {
    quint16 platform, encoding, language, id;
    QByteArray value;
  };
  std::vector<Name> names;
  const auto replaced = [](int id) {
    return id == 1 || id == 2 || id == 3 || id == 4 || id == 6 || id == 16 || id == 17 || id == 21 || id == 22;
  };
  for (auto& table : tables) {
    if (table.tag == 0x6e616d65) {
      if (table.bytes.size() < 6)
        return {};
      const auto nameCount = get16(table.bytes, 2), storage = get16(table.bytes, 4);
      if (6 + 12 * nameCount > table.bytes.size())
        return {};
      for (int i = 0; i < nameCount; ++i) {
        const auto at = 6 + 12 * i;
        const auto id = get16(table.bytes, at + 6), length = get16(table.bytes, at + 8),
                   offset = get16(table.bytes, at + 10);
        if (qsizetype(storage) + offset + length > table.bytes.size())
          return {};
        if (!replaced(id))
          names.push_back({get16(table.bytes, at), get16(table.bytes, at + 2), get16(table.bytes, at + 4), id,
                           table.bytes.mid(storage + offset, length)});
      }
      const QString fullName = family + ' ' + subfamily;
      QString postscript = family;
      postscript.remove(QRegularExpression(QStringLiteral("[^a-zA-Z0-9-]")));
      postscript = postscript.left(45) + '-' + subfamily.simplified().remove(' ');
      const auto append = [&](quint16 id, const QString& value) {
        QByteArray utf16(value.size() * 2, Qt::Uninitialized);
        for (qsizetype i = 0; i < value.size(); ++i)
          put16(utf16, i * 2, value[i].unicode());
        names.push_back({3, 1, 0x409, id, std::move(utf16)});
      };
      append(1, family);
      append(2, subfamily);
      append(3, fullName);
      append(4, fullName);
      append(6, postscript);
      append(16, family);
      append(17, subfamily);
      append(21, family);
      append(22, subfamily);
      std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) {
        return std::tie(a.platform, a.encoding, a.language, a.id) < std::tie(b.platform, b.encoding, b.language, b.id);
      });
      const qsizetype start = 6 + 12 * names.size();
      qsizetype bytes = start;
      for (const auto& name : names)
        bytes += name.value.size();
      if (bytes > 65535)
        return {};
      table.bytes = QByteArray(bytes, '\0');
      put16(table.bytes, 2, names.size());
      put16(table.bytes, 4, start);
      qsizetype offset = 0;
      for (qsizetype i = 0; i < qsizetype(names.size()); ++i) {
        const auto& n = names[i];
        const auto at = 6 + 12 * i;
        put16(table.bytes, at, n.platform);
        put16(table.bytes, at + 2, n.encoding);
        put16(table.bytes, at + 4, n.language);
        put16(table.bytes, at + 6, n.id);
        put16(table.bytes, at + 8, n.value.size());
        put16(table.bytes, at + 10, offset);
        std::copy(n.value.begin(), n.value.end(), table.bytes.begin() + start + offset);
        offset += n.value.size();
      }
    } else if (table.tag == 0x4f532f32 && table.bytes.size() >= 64 && (weight || italic)) {
      put16(table.bytes, 4, actualWeight);
      auto flags = get16(table.bytes, 62) & ~quint16(0x261); // italic, bold, regular, oblique
      if (actualItalic)
        flags |= 1;
      if (actualWeight >= 600)
        flags |= 0x20;
      if (!actualItalic && actualWeight < 600)
        flags |= 0x40;
      put16(table.bytes, 62, flags);
    } else if (table.tag == 0x68656164 && table.bytes.size() >= 46) {
      put32(table.bytes, 8, 0);
      if (weight || italic)
        put16(table.bytes, 44,
              (get16(table.bytes, 44) & ~quint16(3)) | (actualWeight >= 600 ? 1 : 0) | (actualItalic ? 2 : 0));
    }
  }
  // STAT can enumerate other named instances in static exports. DirectWrite
  // must match this private static face by its actual OS/2 descriptors.
  if (std::none_of(tables.begin(), tables.end(), [](const auto& table) { return table.tag == 0x66766172; }))
    std::erase_if(tables, [](const auto& table) { return table.tag == 0x53544154; });
  const auto outputCount = tables.size();
  qsizetype total = 12 + 16 * outputCount;
  for (const auto& table : tables)
    total += (table.bytes.size() + 3) & ~qsizetype(3);
  if (names.empty() || total > maxFontBytes)
    return {};
  if (validateOnly)
    return QByteArray(1, '1');
  QByteArray result(total, '\0');
  std::copy_n(native.begin(), 12, result.begin());
  put16(result, 4, outputCount);
  quint16 power = 1, selector = 0;
  while (power * 2 <= outputCount) {
    power *= 2;
    ++selector;
  }
  put16(result, 6, power * 16);
  put16(result, 8, selector);
  put16(result, 10, outputCount * 16 - power * 16);
  qsizetype offset = 12 + 16 * outputCount, head = -1;
  for (qsizetype i = 0; i < qsizetype(tables.size()); ++i) {
    const auto& table = tables[i];
    const auto at = 12 + 16 * i;
    put32(result, at, table.tag);
    put32(result, at + 4, checksum(table.bytes));
    put32(result, at + 8, offset);
    put32(result, at + 12, table.bytes.size());
    std::copy(table.bytes.begin(), table.bytes.end(), result.begin() + offset);
    if (table.tag == 0x68656164)
      head = offset;
    offset += (table.bytes.size() + 3) & ~qsizetype(3);
  }
  if (head < 0)
    return {};
  put32(result, head + 8, 0xB1B0AFBA - checksum(result));
  return result;
}
} // namespace
ThemeFontRegistrationStats themeFontRegistrationStats() {
  return registrationStats;
}
QByteArray isolateThemeFont(const QByteArray& native, const QString& family, std::optional<int> weight,
                            std::optional<bool> italic) {
  return transformThemeFont(native, family, weight, italic, false);
}
PreparedThemeFont prepareThemeFont(const QByteArray& source, std::optional<int> weight, std::optional<bool> italic) {
  auto native = decodeThemeWebFont(source);
  if (transformThemeFont(native, "MuffinValidation", weight, italic, true).isEmpty())
    return {};
  diag::ScopedPerfProbe perf("font.contentDigest", fontLoadPerf());
  auto digest = themeContentDigest(native);
  return {std::move(native), std::move(digest)};
}
QString registerPrivateFont(const PreparedThemeFont& source, const QString& family, std::optional<int> weight,
                            std::optional<bool> italic) {
  diag::ScopedPerfProbe perf("font.register", fontLoadPerf());
  static QHash<QByteArray, QString> registered;
  if (source.native.isEmpty() || source.digest.isEmpty())
    return {};
  // Family identity and registration share the selected content digest, avoiding
  // a second full-font SHA and a temporary concatenation of a large CJK face.
  const auto key =
      QCryptographicHash::hash(source.digest + family.toUtf8() + '/' + QByteArray::number(weight.value_or(-1)) + '/' +
                                   QByteArray::number(italic ? int(*italic) : -1),
                               QCryptographicHash::Sha256);
  if (const auto found = registered.constFind(key); found != registered.cend())
    return *found;
  const auto native = isolateThemeFont(source.native, family, weight, italic);
  const int id = [&] {
    diag::ScopedPerfProbe qtPerf("font.qtRegister", fontLoadPerf());
    return native.isEmpty() ? -1 : QFontDatabase::addApplicationFontFromData(native);
  }();
  if (id < 0)
    return {};
  const auto families = QFontDatabase::applicationFontFamilies(id);
  if (families.isEmpty())
    return {};
  ++registrationStats.faces;
  registrationStats.nativeBytesSubmitted += native.size();
  registered.insert(key, families.front());
  return families.front();
}
QString registerPrivateFont(const QByteArray& source, const QString& family, std::optional<int> weight,
                            std::optional<bool> italic) {
  return registerPrivateFont(prepareThemeFont(source, weight, italic), family, weight, italic);
}
} // namespace muffin

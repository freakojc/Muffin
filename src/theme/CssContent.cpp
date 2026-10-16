#include "theme/CssContent.h"

#include "theme/CssThemeParser.h"

#include <QChar>
#include <QStringList>
#include <QRegularExpression>
#include "theme/CssValueParser.h"
#include <QtGlobal>
#include <algorithm>

namespace muffin {

std::optional<QVector<QPair<QString, int>>> parseCounterOperations(const QString& text, int defaultValue) {
  QVector<QPair<QString,int>> result;
  const auto parts = splitTopLevelSpaces(text.trimmed());
  if (parts.isEmpty() || text.trimmed().compare("none", Qt::CaseInsensitive) == 0) return result;
  static const QRegularExpression identifier(QStringLiteral("^-?(?:[_a-zA-Z\\x{0080}-\\x{FFFF}])(?:[-_a-zA-Z0-9\\x{0080}-\\x{FFFF}])*$"));
  static const QRegularExpression integer(QStringLiteral("^[+-]?[0-9]+$"));
  for (int i = 0; i < parts.size(); ++i) {
    const auto name = parts[i];
    if (!identifier.match(name).hasMatch() || QStringList{"none","initial","inherit","unset","revert","revert-layer","default"}.contains(name.toLower())) return {};
    int value = defaultValue;
    if (i+1 < parts.size() && integer.match(parts[i+1]).hasMatch()) {
      bool valid = false; value = parts[++i].toInt(&valid); if (!valid) return {};
    }
    result.append({name,value});
  }
  return result;
}

QString formatCounterValue(int value, const QString& style) {
  if (value < 1) { return QString::number(value); }
  if (style == QStringLiteral("decimal-leading-zero")) {
    return QString::number(value).rightJustified(2, QLatin1Char('0'));
  }
  if (style == QStringLiteral("lower-alpha") || style == QStringLiteral("lower-latin")) {
    QString s;
    int v = value;
    while (v > 0) { --v; s.prepend(QChar(QLatin1Char('a').toLatin1() + v % 26)); v /= 26; }
    return s;
  }
  if (style == QStringLiteral("upper-alpha") || style == QStringLiteral("upper-latin")) {
    QString s;
    int v = value;
    while (v > 0) { --v; s.prepend(QChar(QLatin1Char('A').toLatin1() + v % 26)); v /= 26; }
    return s;
  }
  if (style == QStringLiteral("lower-roman") || style == QStringLiteral("upper-roman")) {
    if(value>3999)return QString::number(value);  // CSS roman systems fall back to decimal outside their range.
    static const struct { int v; const char* s; } table[] = {
        {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
        {50, "l"}, {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
    QString s;
    int v = value;
    for (const auto& e : table) {
      while (v >= e.v) { s += QLatin1String(e.s); v -= e.v; }
    }
    return style == QStringLiteral("upper-roman") ? s.toUpper() : s;
  }
  if (style == QStringLiteral("lower-greek")) {
    // Greek lowercase α..ω. U+03C2 (final sigma ς) is NOT part of the CSS
    // lower-greek alphabet, so a naive 0x03B1+i walks into ς at index 17 and
    // shifts every letter from σ onward (σ→ς, τ→σ, …, ω dropped). Skip ς.
    QString s;
    int v = value;
    while (v > 0) {
      --v;
      int cp = 0x03B1 + (v % 24);
      if (cp >= 0x03C2) { ++cp; }  // jump over U+03C2 final sigma
      s.prepend(QChar(char16_t(cp)));
      v /= 24;
    }
    return s;
  }
  return QString::number(value);  // decimal / default
}

namespace {

// CSS strings can be adjacent, contain escaped quotes, or use hexadecimal escapes.
QString readString(const QString& source, qsizetype& at, bool& valid) {
  const QChar quote = source[at++];
  QString result;
  while (at < source.size()) {
    QChar ch = source[at++];
    if (ch == quote) { valid = true; return result; }
    if (ch == '\\') {
      if (at >= source.size()) break;
      ch = source[at++];
      if (ch == '\n' || ch == '\r' || ch == '\f') {
        if (ch == '\r' && at < source.size() && source[at] == '\n') ++at;
        continue;
      }
      const auto hex = [](QChar value) { return value.isDigit() && value.unicode() < 128 ||
          (value.toLower() >= 'a' && value.toLower() <= 'f'); };
      if (hex(ch)) {
        QString digits(ch);
        for (int n = 1; n < 6 && at < source.size() && hex(source[at]); ++n) digits += source[at++];
        if (at < source.size() && source[at].isSpace()) {
          const auto space = source[at++];
          if (space == '\r' && at < source.size() && source[at] == '\n') ++at;
        }
        char32_t cp = digits.toUInt(nullptr, 16);
        if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
        result += QString::fromUcs4(&cp, 1);
        continue;
      }
    }
    result += ch;
  }
  valid = false;
  return {};
}

QString unquote(const QString& source) {
  const auto value = source.trimmed();
  if (value.isEmpty() || (value.front() != '"' && value.front() != '\'')) return value;
  qsizetype at = 0; bool valid = false;
  const auto result = readString(value, at, valid);
  return valid && at == value.size() ? result : QString();
}

}  // namespace

std::vector<ContentToken> parseContentTokens(const QString& content) {
  std::vector<ContentToken> out;
  qsizetype at = 0;
  while (at < content.size()) {
    if (content[at].isSpace()) { ++at; continue; }
    if (content.mid(at, 2) == "/*") {
      const auto end = content.indexOf("*/", at + 2);
      if (end < 0) return {};
      at = end + 2; continue;
    }
    if (content[at] == '/' ) break;  // alternative accessibility text is not visual content
    if (content[at] == '"' || content[at] == '\'') {
      bool valid = false;
      const auto text = readString(content, at, valid);
      if (!valid) return {};
      ContentToken token; token.text = text; out.push_back(std::move(token));
      continue;
    }
    const auto start = at;
    while (at < content.size() && !content[at].isSpace() && content[at] != '(' && content[at] != '"' && content[at] != '\'') ++at;
    const auto name = content.mid(start, at - start).toLower();
    if (at == start) return {};
    if (at >= content.size() || content[at] != '(') {
      if (name != "none" && name != "normal") { ContentToken t; t.text = name; out.push_back(std::move(t)); }
      continue;
    }
    const auto open = at++;
    int depth = 1;
    while (at < content.size() && depth) {
      if (content[at] == '"' || content[at] == '\'') {
        bool valid = false; readString(content, at, valid);
        if (!valid) return {};
      } else {
        if (content[at] == '(') ++depth;
        if (content[at] == ')') --depth;
        ++at;
      }
    }
    if (depth) return {};
    const auto args = CssThemeParser::splitTopLevelCommas(content.mid(open + 1, at - open - 2));
    ContentToken token;
    if (name == "attr") {
      if (args.isEmpty() || args.size() > 2) return {};
      const auto descriptor = args[0].simplified().split(' ');
      if (descriptor.isEmpty() || descriptor.size() > 2 ||
          (descriptor.size() == 2 && descriptor[1] != "string" && descriptor[1] != "raw-string")) return {};
      token.kind = ContentToken::Kind::Attribute;
      token.text = descriptor[0].toLower();
      if (token.text.isEmpty() || !std::all_of(token.text.cbegin(), token.text.cend(), [](QChar ch) {
          return ch.isLetterOrNumber() || ch == '-' || ch == '_' || ch == ':'; })) return {};
      if (args.size() == 2) token.fallback = unquote(args[1]);
    } else if (name == "counter" || name == "counters") {
      token.kind = name == "counter" ? ContentToken::Kind::Counter : ContentToken::Kind::Counters;
      if (!args.isEmpty()) token.text = args[0].trimmed();
      if (name == "counter" && args.size() >= 2) token.style = args[1].trimmed().toLower();
      if (name == "counters" && args.size() >= 2) token.sep = unquote(args[1]);
      if (name == "counters" && args.size() >= 3) token.style = args[2].trimmed().toLower();
    } else token.text = content.mid(start, at - start);  // existing image content is handled by the icon projection
    out.push_back(std::move(token));
  }
  return out;
}

QString resolveContentTokens(const std::vector<ContentToken>& tokens,
                             const std::function<int(const QString&)>& value,
                             const std::function<QVector<int>(const QString&)>& chain,
                             const std::function<std::optional<QString>(const QString&)>& attribute) {
  QString out;
  for (const ContentToken& t : tokens) {
    if (t.kind == ContentToken::Kind::Literal) {
      out += t.text;
    } else if (t.kind == ContentToken::Kind::Counter) {
      out += formatCounterValue(value(t.text), t.style);
    } else if (t.kind == ContentToken::Kind::Attribute) {
      const auto result = attribute ? attribute(t.text) : std::nullopt;
      out += result ? *result : t.fallback;
    } else {  // Counters
      const QVector<int> levels = chain(t.text);
      for (int li = 0; li < levels.size(); ++li) {
        if (li > 0) { out += t.sep; }
        out += formatCounterValue(levels.at(li), t.style);
      }
    }
  }
  return out;
}

}  // namespace muffin

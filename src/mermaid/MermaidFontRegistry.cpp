#include "mermaid/MermaidFontRegistry.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QRawFont>
#include <QSet>
#include <algorithm>

#include "theme/ThemeFontDecoder.h"

static void initMermaidFontsResource() { Q_INIT_RESOURCE(mermaid_fonts); }

namespace muffin::mermaid {
namespace {

QStringList& loadedFamilies() {
  static QStringList families;
  return families;
}
QHash<QString, QString>& bundledAliases() {
  static QHash<QString, QString> aliases;
  return aliases;
}

}  // namespace

void MermaidFontRegistry::ensureLoaded() {
  static const bool loaded = [] {
    initMermaidFontsResource();
    const QStringList resources = {
        QStringLiteral(":/mermaid/fonts/NotoSans-Regular.ttf"),
        QStringLiteral(":/mermaid/fonts/NotoSansCJKsc-Regular.otf"),
        QStringLiteral(":/mermaid/fonts/NotoSansArabic-Regular.ttf"),
        QStringLiteral(":/mermaid/fonts/NotoSansHebrew-Regular.ttf"),
    };
    QSet<QString> seen;
    QStringList& families = loadedFamilies();
    for (const QString& resource : resources) {
      QFile file(resource);
      if (!file.open(QIODevice::ReadOnly)) qFatal("Missing bundled Mermaid font: %s", qPrintable(resource));
      const auto source = file.readAll();
      const auto logical = QFileInfo(resource)
                               .fileName()
                               .split('-')
                               .front()
                               .replace("NotoSansCJKsc", "Noto Sans CJK SC")
                               .replace("NotoSansArabic", "Noto Sans Arabic")
                               .replace("NotoSansHebrew", "Noto Sans Hebrew")
                               .replace("NotoSans", "Noto Sans");
      const auto family = registerPrivateFont(
          source,
          logical + " Muffin-" +
              QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex().left(16)));
      if (family.isEmpty()) {
        // Some Qt platform plugins (notably minimal) have no application-font
        // support. Keep their native fallback usable for headless workflows.
        qWarning("Current Qt platform cannot register bundled Mermaid fonts");
        return true;
      }
      bundledAliases().insert(logical.toLower(), family);
      {
        if (!seen.contains(family)) {
          seen.insert(family);
          families.append(family);
        }
      }
    }
#ifdef Q_OS_WIN
    // Mermaid's Event Modeling helper measures with the browser's fixed CSS
    // stack headed by Trebuchet MS. Conan Qt does not discover Windows system
    // fonts in the offscreen/runtime bundle, while Chromium does. Register the
    // installed faces so QFont's CSS fallback list resolves the same face. Do
    // not add them to familyStack(): Noto remains Muffin's deterministic root
    // font and these faces are selected only when a diagram asks for them.
    const QString windowsFonts =
        qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows")) + QStringLiteral("/Fonts/");
    const QStringList compatibilityFaces = {QStringLiteral("trebuc.ttf"), QStringLiteral("trebucbd.ttf"),
                                            QStringLiteral("trebucit.ttf"), QStringLiteral("trebucbi.ttf")};
    for (const QString& face : compatibilityFaces) {
      const QString path = windowsFonts + face;
      if (QFileInfo::exists(path)) QFontDatabase::addApplicationFont(path);
    }
#endif
    return true;
  }();
  (void)loaded;
}

QString MermaidFontRegistry::primaryFamily() {
  ensureLoaded();
  return QStringLiteral("Noto Sans");
}

QStringList MermaidFontRegistry::familyStack() {
  ensureLoaded();
  return loadedFamilies();
}
QString MermaidFontRegistry::bundledFamily(const QString& declaredFamily) {
  ensureLoaded();
  return bundledAliases().value(declaredFamily.toLower());
}
QStringList MermaidFontRegistry::resolveFamilies(QStringList families) {
  ensureLoaded();
  for (auto& family : families) family = bundledAliases().value(family.toLower(), family);
  return families;
}

QString MermaidFontRegistry::cssFamilyStack() {
  return QStringLiteral(
      "\"Noto Sans\", \"Noto Sans CJK SC\", "
      "\"Noto Sans Arabic\", \"Noto Sans Hebrew\", sans-serif");
}

void MermaidFontRegistry::configureFont(QFont& font, const QString& familyExpression) {
  ensureLoaded();
  if (!familyExpression.contains(QStringLiteral("Noto Sans"), Qt::CaseInsensitive)) return;

  QStringList ordered;
  for (QString family : familyExpression.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    family = family.trimmed();
    if (family.size() >= 2 && ((family.front() == QLatin1Char('"') && family.back() == QLatin1Char('"')) ||
                               (family.front() == QLatin1Char('\'') && family.back() == QLatin1Char('\'')))) {
      family = family.mid(1, family.size() - 2);
    }
    const auto resolved = bundledAliases().value(family.toLower(), family);
    if (!resolved.isEmpty() && !ordered.contains(resolved, Qt::CaseInsensitive)) ordered.append(resolved);
  }
  for (const QString& family : loadedFamilies())
    if (!ordered.contains(family)) ordered.append(family);
  if (!ordered.isEmpty()) {
    font.setFamilies(ordered);
    if (!loadedFamilies().contains(ordered.front(), Qt::CaseInsensitive)) {
      // Metric code must observe the selected primary face when an earlier
      // CSS name is missing. Keep the remaining list for per-glyph fallback.
      const auto raw = QRawFont::fromFont(font);
      if (raw.isValid() && !raw.familyName().isEmpty() &&
          raw.familyName().compare(ordered.front(), Qt::CaseInsensitive) != 0) {
        ordered.removeAll(raw.familyName());
        ordered.prepend(raw.familyName());
        font.setFamilies(ordered);
      }
    }
  }
}

}  // namespace muffin::mermaid

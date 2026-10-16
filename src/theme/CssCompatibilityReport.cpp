#include "theme/CssCompatibilityReport.h"

#include "theme/CssComputedStyleEngine.h"
#include "theme/CssContent.h"
#include "theme/CssValueParser.h"
#include "theme/CssImageResources.h"
#include "theme/CssBlendMode.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <tuple>

namespace muffin {
namespace {

// Bound examples, including minified SVG/base64 values. The full-value digest
// distinguishes resources with the same preview without copying their payload.
QString preview(const QString& value) {
  constexpr int limit = 180;
  QString out;
  out.reserve(limit + 1);
  for (const auto c : value) {
    if (out.size() == limit) { out += QChar(0x2026); break; }
    out += c.isSpace() ? QChar(' ') : c;
  }
  return out;
}

bool cssWide(const QString& value) {
  return value == "inherit" || value == "initial" || value == "unset" || value == "revert" || value == "revert-layer";
}

struct Context {
  CssSourceLocation source;
  QString selector;
  QStringList media;
  QStringList supports;
  QString scope = QStringLiteral("active-rule");
};

struct Finding {
  QString code, severity, property, value, scope, message;
  int occurrences = 0;
  QJsonArray examples;
};

struct Audit {
  QString baseDir;
  CssEnvironment environment;
  std::vector<Finding> findings;
  QHash<QString, int> index;
  QSet<QString> sources;
  QHash<QString, bool> supportResults;

  bool supportMatches(const QString& query) {
    auto it = supportResults.constFind(query);
    if (it == supportResults.cend()) it = supportResults.insert(query, CssThemeSheet::supportsMatches(query));
    return *it;
  }

  bool conditionsMatch(const Context& context) {
    for (const auto& media : context.media)
      if (!CssThemeSheet::mediaMatches(media, environment)) return false;
    for (const auto& supports : context.supports)
      if (!supportMatches(supports)) return false;
    return true;
  }

  void add(const QString& code, const QString& severity, const QString& property, const QString& value,
           const Context& context, const QString& message, bool groupByValue = true) {
    const auto digest = groupByValue ? QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex())
                                     : QString{};
    const QString key = code + '\n' + property + '\n' + context.scope + '\n' + digest;
    auto it = index.constFind(key);
    if (it == index.cend()) {
      it = index.insert(key, int(findings.size()));
      findings.push_back({code, severity, property, groupByValue ? preview(value) : QString{}, context.scope, message});
    }
    auto& finding = findings[*it];
    ++finding.occurrences;
    if (finding.examples.size() < 3) {
      QJsonObject example{{"file", context.source.file}, {"line", context.source.line}, {"column", context.source.column},
                          {"selector", preview(context.selector)}, {"value", preview(value)},
                          {"media", QJsonArray::fromStringList(context.media)}, {"supports", QJsonArray::fromStringList(context.supports)}};
      if (value.size() > 180) example.insert("valueSha256", QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex()));
      finding.examples.append(example);
    }
  }

  QString localPath(const QString& target, const Context& context) const {
    const auto dir = context.source.file.isEmpty() ? baseDir : QFileInfo(context.source.file).absolutePath();
    return CssImageResources::localPath(target, dir);
  }

  void resource(const QString& target, const QString& role, const Context& context, bool generatedPseudo = false) {
    const auto lower = target.toLower();
    if (lower.startsWith("data:")) {
      if (role == "import")
        add("embedded-resource-skipped", "warning", role, target, context, "Embedded stylesheets are not loaded.");
      else if(role=="font")
        add("font-registration-unverified","info",role,target,context,"Embedded fonts can be decoded; Qt registration and family fallback require a GUI runtime check.");
      else if (lower.startsWith("data:image/svg+xml") && generatedPseudo) {
        // The generated-content painter has its own SVG icon path.
      } else if (!CssImageResources::supportsUrl(target))
        add("unsupported-image-format", "warning", role, target, context,
            "The image data URL has an unsupported MIME type.");
      else if (role == "content")
        add("unsupported-generated-image", "warning", role, target, context, "Raster generated content has no replaced-image layout path.");
      return;
    }
    const QUrl url(target);
    const bool remote = target.startsWith("//") || (!url.isLocalFile() && !url.scheme().isEmpty() && !QDir::isAbsolutePath(target) && !target.startsWith(':'));
    if (remote) {
      add("remote-resource-skipped", "warning", role, target, context, "Remote resources are not fetched by the native theme renderer.");
      return;
    }
    if (target.startsWith('#')) {
      add("fragment-resource-unverified", "info", role, target, context, "Document fragment resources require a live element context.");
      return;
    }
    const auto path = localPath(target, context);
    if (!QFileInfo(path).isFile()) {
      add("missing-local-resource", "warning", role, target, context, "The local resource is missing or is not a regular file.");
    } else if (role == "background" || role == "background-image" || role.startsWith("mask") || role.startsWith("-webkit-mask") ||
               (generatedPseudo && target.endsWith(".svg", Qt::CaseInsensitive) &&
                (role == "content" || role.startsWith("mask") || role.startsWith("-webkit-mask")))) {
      add("image-decoding-unverified", "info", role, target, context,
          "The image exists and can be loaded lazily; this offline audit does not decode it or verify its final pixels.");
    } else if (role != "font" && role != "import") {
      add("external-image-skipped", "warning", role, target, context, "The background/generated-content path does not load external image files.");
    } else if (role == "font") {
      add("font-registration-unverified", "info", role, target, context,
          "The font file exists; decoding, Qt registration and family fallback require a GUI runtime check.");
    }
  }

  void declaration(const CssDeclaration& declaration, const Context& context, bool generatedPseudo) {
    const auto& property = declaration.property;
    const auto& value = declaration.value;
    if (property.startsWith("--")) return;
    const auto lower = value.trimmed().toLower();
    const auto warn = [&](const QString& code, const QString& message) { add(code, "warning", property, value, context, message); };
    const auto note = [&](const QString& code, const QString& message) { add(code, "info", property, value, context, message, false); };
    if (lower.contains("var(")) {
      note("context-dependent-value", "Custom properties need the live element cascade; this declaration was not classified as unsupported.");
      return;
    }
    if (property == "mix-blend-mode") {
      if (cssWide(lower)) return;
      const auto modes = CssThemeParser::splitTopLevelCommas(lower);
      if (std::any_of(modes.begin(), modes.end(), [](const auto& mode) { return mode != "normal" && mode != "initial"; }))
        warn("unsupported-blend-mode", "Element/subtree mix-blend-mode is not implemented; background-blend-mode is supported separately.");
      return;
    }
    const auto urls = CssThemeParser::resourceUrls(value);
    for (const auto& target : urls) resource(target, property, context, generatedPseudo);
    static const QRegularExpression attachment(QStringLiteral("(?:^|[\\s,])(?:fixed|local)(?:$|[\\s,])"));
    if ((property == "background" || property == "background-attachment") && attachment.match(lower).hasMatch())
      warn("background-attachment-limits", "Backgrounds use the element paint origin; fixed/local attachment semantics are incomplete.");
    if (property == "background-clip" && lower.contains("text"))
      warn("unsupported-background-clip", "Text clipping is not implemented for background layers.");
    if (property == "position" && (lower == "sticky" || lower == "fixed"))
      warn("positioning-limits", "Sticky and viewport-fixed positioning are not implemented by the document layout.");
    if (property.startsWith("counter-") && lower.contains("reversed("))
      warn("counter-scope-limits", "Reversed counters with automatic initial values are not implemented.");
    if (property == "position" && lower == "absolute")
      note("positioning-limits", "Block-host generated pseudos use the host padding box. Inline absolute pseudos and arbitrary positioned descendants require a full containing-block tree.");
    if ((property == "mask-image" || property == "mask" || property == "-webkit-mask-image") && lower.contains('#') && !urls.isEmpty())
      note("mask-resource-limits", "SVG images are supported; SVG mask fragment references and match-source mask-type require additional SVG resource semantics.");
    if (property == "content" && urls.isEmpty()) {
      if (lower != "none" && lower != "normal" && !cssWide(lower) && parseContentTokens(value).empty())
        warn("unsupported-content-value", "Generated content supports strings, string attr() and counters; typed/numeric attr() is unsupported.");
      return;
    }
    // A false feature query may mean a missing validator, rather than a missing
    // painter. Probe a CSS-wide keyword to tell those cases apart. Resource
    // capabilities depend on the pseudo host and are audited above instead.
    if (!urls.isEmpty()) return;
    if (CssComputedStyleEngine::supportsDeclaration(property, value)) return;
    if (CssComputedStyleEngine::supportsDeclaration(property, "initial"))
      warn("unsupported-property-value", "This value is rejected by the shared declaration capability validator.");
    else
      note("unverified-property", "No complete capability validator exists for this property; this is not a claim that the renderer ignores it.");
  }
};

}  // namespace

CssCompatibilityReport CssCompatibilityReport::analyze(const CssThemeSheet& sheet, const QString& baseDir, const CssEnvironment& environment) {
  Audit audit{baseDir, environment};
  int activeRules = 0, inactiveRules = 0, supportedSelectors = 0, unsupportedSelectors = 0, excludedSelectors = 0;
  QHash<QString, CssSelectorSupport> selectorResults;
  QHash<QString, QString> selectorPseudos;
  for (const auto& rule : sheet.rules()) {
    if (!rule.source.file.isEmpty()) audit.sources.insert(rule.source.file);
    Context context{rule.source, {}, rule.mediaQueries, rule.supportQueries};
    const bool active = (!rule.darkScope || environment.dark) && audit.conditionsMatch(context);
    if (active) ++activeRules; else ++inactiveRules;
    if (!active) context.scope = "inactive-condition";
    bool supported = false, blocked = false, allGenerated = true;
    QString supportedSelector;
    for (const auto& selector : rule.selectors) {
      if (!selectorResults.contains(selector)) {
        QString pseudo;
        selectorResults.insert(selector, CssComputedStyleEngine::selectorSupport(selector, &pseudo));
        selectorPseudos.insert(selector, pseudo);
      }
      context.selector = selector;
      switch (selectorResults.value(selector)) {
        case CssSelectorSupport::Supported: {
          ++supportedSelectors;
          const auto pseudo = selectorPseudos.value(selector);
          const bool generated = pseudo == "before" || pseudo == "after";
          if (supportedSelector.isEmpty() || !generated) supportedSelector = selector;
          allGenerated = allGenerated && generated;
          supported = true;
          break;
        }
        case CssSelectorSupport::Unsupported:
          ++unsupportedSelectors;
          blocked = true;
          audit.add("unsupported-selector", "warning", {}, selector, context, "The selector compiler or document pseudo painter cannot evaluate this selector.");
          break;
        default: {
          ++excludedSelectors;
          Context excludedContext = context;
          excludedContext.scope = "excluded-context";
          audit.add("excluded-selector", "info", {}, selector, excludedContext, "Typora editor chrome/export-only selectors are deliberately excluded from document styling.", false);
          break;
        }
      }
    }
    context.selector = supported ? supportedSelector : rule.selectors.value(0);
    if (!supported && active) context.scope = blocked ? "blocked-selector" : "excluded-context";
    for (const auto& declaration : rule.declarations) audit.declaration(declaration, context, supported && allGenerated);
  }
  for (const auto& rule : sheet.atRules()) {
    if (!rule.source.file.isEmpty()) audit.sources.insert(rule.source.file);
    Context context{rule.source, '@' + rule.keyword, rule.mediaQueries, rule.supportQueries};
    if (!audit.conditionsMatch(context)) context.scope = "inactive-condition";
    if (rule.keyword == "supports") {
      if (!audit.supportMatches(rule.prelude))
        audit.add("inactive-supports", "info", "@supports", rule.prelude, context, "This capability query is false or invalid; its branch is excluded from the cascade.");
    } else if (rule.keyword == "media" || rule.keyword == "charset" || rule.keyword == "keyframes") {
      // Recognized by the parser (charset is harmless after UTF-8 decoding).
    } else if (rule.keyword == "import") {
      if (!rule.resourceTarget.isEmpty()) audit.resource(rule.resourceTarget, "import", context);
      if (!rule.importModifiers.isEmpty() || !context.media.isEmpty() || !context.supports.isEmpty())
        audit.add("import-condition-limits", "warning", "@import", rule.prelude, context,
                  "The import loader does not apply media/supports/layer modifiers or nested imports.");
    } else if (rule.keyword == "font-face") {
      for (const auto& descriptor : rule.descriptors)
        if (descriptor.property == "src") {
          for (const auto& target : CssThemeParser::resourceUrls(descriptor.value)) audit.resource(target, "font", context);
          if (descriptor.value.contains("local(", Qt::CaseInsensitive))
            audit.add("local-font-source-unverified", "info", "font", descriptor.value, context,
                      "local() is not registered as a theme font; system family lookup is left to Qt.", false);
        }
      if (!rule.mediaQueries.isEmpty())
        audit.add("font-media-condition-limits", "warning", "@font-face", {}, context, "Font registration currently does not evaluate enclosing media queries.");
    } else {
      audit.add("unsupported-at-rule", "warning", '@' + rule.keyword, rule.prelude, context, "The parser skips this at-rule and its body.");
    }
  }
  std::sort(audit.findings.begin(), audit.findings.end(), [](const auto& a, const auto& b) {
    return std::tie(a.code, a.property, a.scope, a.value) < std::tie(b.code, b.property, b.scope, b.value);
  });
  QJsonArray findings;
  QJsonObject codes;
  int warnings = 0, activeWarnings = 0;
  for (const auto& finding : audit.findings) {
    findings.append(QJsonObject{{"code", finding.code}, {"severity", finding.severity}, {"property", finding.property},
                               {"value", finding.value}, {"scope", finding.scope}, {"message", finding.message},
                               {"occurrences", finding.occurrences}, {"examples", finding.examples}});
    codes.insert(finding.code, codes.value(finding.code).toInt() + finding.occurrences);
    if (finding.severity == "warning") {
      warnings += finding.occurrences;
      if (finding.scope == "active-rule") activeWarnings += finding.occurrences;
    }
  }
  auto files = audit.sources.values();
  files.sort();
  CssCompatibilityReport result;
  result.report_ = {{"schemaVersion", 1}, {"kind", "offline-css-capability-audit"},
                    {"limitations", "Findings describe authored rules, not winning declarations or matching document nodes. No finding does not imply Typora/browser pixel parity. Context-dependent and unverified values are separate from confirmed gaps. Fonts are not registered and resources are not fetched."},
                    {"environment", QJsonObject{{"viewportWidth", environment.viewportWidth}, {"viewportHeight", environment.viewportHeight},
                       {"dark", environment.dark}, {"print", environment.print}, {"resolutionDppx", environment.resolutionDppx},
                       {"reducedMotion", environment.reducedMotion}, {"hoverAvailable", environment.hoverAvailable}, {"pointer", environment.pointer}}},
                    {"sources", QJsonArray::fromStringList(files)},
                    {"summary", QJsonObject{{"rules", int(sheet.rules().size())}, {"activeRules", activeRules}, {"inactiveRules", inactiveRules},
                       {"supportedSyntaxSelectors", supportedSelectors}, {"unsupportedSelectors", unsupportedSelectors}, {"excludedSelectors", excludedSelectors},
                       {"findingGroups", int(findings.size())}, {"warningOccurrences", warnings}, {"activeWarningOccurrences", activeWarnings}, {"codes", codes}}},
                    {"findings", findings}};
  return result;
}

}  // namespace muffin

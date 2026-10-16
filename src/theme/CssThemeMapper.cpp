#include "theme/CssThemeMapper.h"
#include "diagnostics/ScopedPerfProbe.h"
#include "theme/CssImageResources.h"

#include "theme/CssCalc.h"
#include "theme/CssContent.h"
#include "theme/CssComputedStyleEngine.h"
#include "theme/CssSelectorUtils.h"
#include "theme/CssStyleDebug.h"
#include "theme/CssValueParser.h"
#include "theme/CssDecorationExtractor.h"
#include "theme/DocumentStyleTree.h"
#include "theme/ThemeDefinition.h"
#include "theme/CssThemeParser.h"

#include <QColor>
#include <QFont>
#include <QLoggingCategory>
#include <QPointF>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <vector>

namespace muffin {

// themeWarn (muffin.theme.warn) is declared in theme/CssValueParser.h and defined in
// CssValueParser.cpp alongside extractColor, the funnel that emits its warnings.

// cssColor lives in theme/CssValueParser.h (shared with HtmlBoxBuilder so hex-alpha colours
// parse identically in theme CSS and HTML inline styles).

namespace {
Q_LOGGING_CATEGORY(mapperLoadPerf, "muffin.perf.detail", QtWarningMsg)

constexpr qreal kUnboundedPageWidth = 100000.0;

bool isIntrinsicPageWidthKeyword(const QString& value) {
  return value == QStringLiteral("fit-content") || value == QStringLiteral("min-content") || value == QStringLiteral("max-content") ||
         value == QStringLiteral("none") || value == QStringLiteral("stretch");
}

qreal relativeLuminance(const QColor& color) {
  const auto linear = [](qreal channel) {
    return channel <= 0.04045 ? channel / 12.92 : qPow((channel + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(color.redF()) +
         0.7152 * linear(color.greenF()) +
         0.0722 * linear(color.blueF());
}

QColor highestContrastInk(const QColor& background) {
  // Black has contrast (L + .05) / .05; white has 1.05 / (L + .05).
  return relativeLuminance(background) > 0.179 ? QColor(Qt::black) : QColor(Qt::white);
}

qreal contrastRatio(const QColor& a, const QColor& b) {
  const qreal lighter = qMax(relativeLuminance(a), relativeLuminance(b));
  const qreal darker = qMin(relativeLuminance(a), relativeLuminance(b));
  return (lighter + 0.05) / (darker + 0.05);
}

QColor typoraHostInk(const QColor& background) {
  const QColor candidate = relativeLuminance(background) > 0.179
      ? QColor(QStringLiteral("#333333"))
      : QColor(QStringLiteral("#dddddd"));
  return contrastRatio(candidate, background) >= 4.5 ? candidate : highestContrastInk(background);
}

struct ParsedBoxShadow {
  QColor color;
  qreal offsetX = 0.0;
  qreal offsetY = 0.0;
  qreal blur = 0.0;
  qreal spread = 0.0;
  bool present = false;
};

ParsedBoxShadow parseFirstBoxShadow(const QString& raw,
                                    const QHash<QString, QString>& vars,
                                    qreal emPx) {
  ParsedBoxShadow out;
  const QString resolved = CssThemeParser::resolveVars(raw, vars).trimmed();
  if (resolved.isEmpty() || resolved.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
    return out;
  }

  const QString first = CssThemeParser::splitTopLevelCommas(resolved).value(0).trimmed();
  out.color = extractColor(first, vars);
  static const QRegularExpression lengthToken(
      QStringLiteral(R"(^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[a-zA-Z%]+)?$)"));
  QVector<qreal> lengths;
  for (const QString& token : splitTopLevelSpaces(first)) {
    const QString value = token.trimmed();
    if (value.compare(QStringLiteral("inset"), Qt::CaseInsensitive) == 0) { continue; }
    if (!lengthToken.match(value).hasMatch()) { continue; }
    lengths.push_back(lengthToPx(value, vars, emPx));
  }
  if (lengths.size() < 2 || !out.color.isValid()) { return out; }
  out.offsetX = lengths.at(0);
  out.offsetY = lengths.at(1);
  if (lengths.size() > 2) { out.blur = qMax<qreal>(0.0, lengths.at(2)); }
  if (lengths.size() > 3) { out.spread = lengths.at(3); }
  out.present = true;
  return out;
}

// Preserve CSS quotes: a quoted generic is a literal family, and a comma can
// be part of a quoted family name. The shared font-stack resolver owns parsing.
QString firstFamily(const QString& value, const QHash<QString, QString>& vars) {
  const QString resolved = CssThemeParser::resolveVars(value, vars).trimmed();
  const QStringList parts = CssThemeParser::splitTopLevelCommas(resolved);
  QStringList out;
  for (const QString& p : parts) {
    QString f = p.trimmed();
    f = f.trimmed();
    if (!f.isEmpty()) { out << f; }
  }
  return out.join(QLatin1Char('\n'));
}

bool fontStackLooksSerif(const QString& stack) {
  for (const QString& raw : stack.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
    const QString f = raw.trimmed().toLower();
    if (f == QStringLiteral("serif") || f.contains(QStringLiteral("serif")) ||
        f.contains(QStringLiteral("times")) || f.contains(QStringLiteral("palatino")) ||
        f.contains(QStringLiteral("georgia")) || f.contains(QStringLiteral("garamond")) ||
        f.contains(QStringLiteral("baskerville")) || f.contains(QStringLiteral("vollkorn")) ||
        f.contains(QStringLiteral("cambria"))) {
      return true;
    }
  }
  return false;
}

Qt::Alignment parseTextAlign(const QString& raw, const QHash<QString, QString>& vars) {
  const QString v = CssThemeParser::resolveVars(raw, vars).trimmed().toLower();
  if (v == QStringLiteral("left") || v == QStringLiteral("start")) { return Qt::AlignLeft; }
  if (v == QStringLiteral("right") || v == QStringLiteral("end")) { return Qt::AlignRight; }
  if (v == QStringLiteral("center")) { return Qt::AlignHCenter; }
  if (v == QStringLiteral("justify")) { return Qt::AlignJustify; }
  return Qt::Alignment();
}

// Qt 6 uses the CSS 1..1000 weight scale, including intermediate values.
int cssWeightToQt(int cssWeight) {
  return qBound(1,cssWeight,1000);
}

// CSS2.1 bolder/lighter: the resulting CSS weight (100-900) given the inherited one.
int bolderCssWeight(int inheritedCss) {
  if (inheritedCss < 350) return 400;
  if (inheritedCss < 550) return 700;
  return qMax(900,inheritedCss);
}
int lighterCssWeight(int inheritedCss) {
  if (inheritedCss < 550) return qMin(100,inheritedCss);
  if (inheritedCss < 750) return 400;
  return 700;
}

struct ParsedFontWeight { int weight = 0; bool present = false; };
// Relative weights normally arrive resolved by the shared computed-style
// engine. Keep the legacy detached mapper's default-parent behavior.
ParsedFontWeight parseFontWeight(const QString& raw, const QHash<QString, QString>& vars,
                                 int inheritedCssWeight = 400) {
  const QString v = CssThemeParser::resolveVars(raw, vars).trimmed().toLower();
  if (v.isEmpty()) { return {}; }
  if (v == QStringLiteral("normal")) { return {QFont::Normal, true}; }
  if (v == QStringLiteral("bold")) { return {QFont::Bold, true}; }
  if (v == QStringLiteral("bolder")) { return {cssWeightToQt(bolderCssWeight(inheritedCssWeight)), true}; }
  if (v == QStringLiteral("lighter")) { return {cssWeightToQt(lighterCssWeight(inheritedCssWeight)), true}; }
  bool ok = false;
  const qreal numeric = v.toDouble(&ok);
  if (ok && numeric>=1 && numeric<=1000) { return {cssWeightToQt(qRound(numeric)), true}; }
  return {};
}

struct ParsedItalic { bool italic = false; bool present = false; };
ParsedItalic parseFontItalic(const QString& raw, const QHash<QString, QString>& vars) {
  const QString v = CssThemeParser::resolveVars(raw, vars).trimmed().toLower();
  if (v == QStringLiteral("italic") || v == QStringLiteral("oblique")) { return {true, true}; }
  if (v == QStringLiteral("normal")) { return {false, true}; }
  return {};
}

qreal parseLineHeightMultiplier(const QString& raw, const QHash<QString, QString>& vars, qreal fontPx) {
  const QString v = CssThemeParser::resolveVars(raw, vars).trimmed().toLower();
  if (v.isEmpty() || v == QStringLiteral("normal")) { return 0.0; }
  bool ok = false;
  const qreal n = v.toDouble(&ok);
  if (ok && n > 0.0) { return n; }
  if (fontPx <= 0.0) { return 0.0; }
  // rem resolves root-relative inside lengthToPx (16px), so the multiplier round-trips:
  // cssFontPx × (lengthPx / fontPx) = lengthPx at apply time.
  const qreal px = lengthToPx(v, vars, fontPx);
  return px > 0.0 ? px / fontPx : 0.0;
}

QString varValue(const QHash<QString, QString>& vars, const char* name) {
  return CssThemeParser::resolveVars(vars.value(QString::fromLatin1(name)), vars).trimmed();
}

QColor varColor(const QHash<QString, QString>& vars, const char* name) {
  const QString v = varValue(vars, name);
  if (v.isEmpty()) { return QColor(); }
  QColor c = cssColor(v);
  return c.isValid() ? c : QColor();
}

bool truthy(const QString& v) {
  const QString t = v.trimmed().toLower();
  return t == QStringLiteral("1") || t == QStringLiteral("true") || t == QStringLiteral("yes") ||
         t == QStringLiteral("on");
}

QString titleCaseId(const QString& id) {
  if (id.isEmpty()) { return id; }
  QString out;
  bool cap = true;
  for (const QChar c : id) {
    if (c == QLatin1Char('-') || c == QLatin1Char('_') || c.isSpace()) { cap = true; out += QLatin1Char(' '); continue; }
    out += cap ? c.toUpper() : c;
    cap = false;
  }
  return out;
}

}  // namespace

ThemeDefinition CssThemeMapper::fromCss(const QString& cssText, const QString& id, const QString& baseDir) {
  return fromSheet(CssThemeParser::parse(cssText, baseDir), id);
}

GradientSpec CssThemeMapper::parseGradient(const QString& raw, const QHash<QString, QString>& vars) {
  return parseGradientSpec(raw, vars);
}

QColor CssThemeMapper::resolveColor(const QString& value, const QHash<QString, QString>& vars) {
  return extractColor(value, vars);
}

qreal CssThemeMapper::resolveLengthPx(const QString& value, const QHash<QString, QString>& vars) {
  return lengthToPx(value, vars, kRootEmPx);
}

qreal CssThemeMapper::resolveLengthPx(const QString& value, const QHash<QString, QString>& vars,
                                      qreal emPx, qreal containingPx) {
  // Box-relative variant: a `%` resolves against `containingPx` (the host box's
  // own dimension) rather than 1em. For paint-time resolution of pseudo width/
  // height (e.g. `h3::before { height: 61% }` → 61% of the heading rect).
  return lengthToPx(value, vars, emPx, -1.0, containingPx);
}

// Build a ThemeElementStyle from a CssComputedStyle, mirroring what the
// makeElementStyle lambda in fromSheet does. Extracted so the real-tree layout
// path (elementStyleForNode) can map a node's computed style the same way.
// `emPx` is the box-geometry em basis; `fontSizeEmPx` (default emPx) is the
// font-size em basis (parent computed font size); `bodyPx` resolves rem/%.
std::vector<ThemeBackgroundLayer> projectImageLayers(const CssComputedStyle& style,const CssLengthContext& paintContext,
    const std::function<QString(QString)>& resolve) {
  std::vector<ThemeBackgroundLayer> layers;
  auto imageLayers = CssThemeParser::splitTopLevelCommas(resolve("background-image"));
  if (imageLayers.isEmpty()) imageLayers << QStringLiteral("none");  // CSS initial value also owns a layer
  QHash<QString, QStringList> layerValues;
  for (const auto& property : {"background-position", "background-position-x", "background-position-y", "background-size",
                               "background-repeat", "background-origin", "background-clip", "background-blend-mode"})
    layerValues.insert(QLatin1String(property), CssThemeParser::splitTopLevelCommas(resolve(QLatin1String(property))));
  const auto layerValue = [&](const char* property, qsizetype index) {
    const auto& values = layerValues.constFind(QLatin1String(property)).value();
    return values.isEmpty() ? QString() : values[index % values.size()].trimmed();
  };
  // Lists cycle independently; a `none` image still occupies a CSS layer.
  for (qsizetype index = 0; index < imageLayers.size(); ++index) {
    ThemeBackgroundLayer layer;
    QString image = imageLayers[index].trimmed();
    layer.image = parseGradientSpec(image, style.customProperties(), paintContext);
    const auto urls = CssThemeParser::resourceUrls(image);
    if (urls.size() == 1 && style.imageResources) {
      layer.resource = style.imageResources->load(urls.front());
      if (layer.resource) layer.bitmap = layer.resource->bitmap;
    }
    const auto position = parseCssPosition(layerValue("background-position", index), paintContext);
    layer.positionX = position.first;
    layer.positionY = position.second;
    for (const auto* axis : {"background-position-x", "background-position-y"}) {
      const auto value = layerValue(axis, index);
      if (!value.isEmpty() && value != "initial") {
        const bool x = QLatin1String(axis) == QLatin1String("background-position-x");
        const auto parsed = parseCssPosition(x ? value + " top" : "left " + value, paintContext);
        (x ? layer.positionX : layer.positionY) = x ? parsed.first : parsed.second;
      }
    }
    const auto sizeValue = layerValue("background-size", index).toLower();
    if (sizeValue == "cover") layer.sizeMode = ThemeBackgroundLayer::SizeMode::Cover;
    else if (sizeValue == "contain") layer.sizeMode = ThemeBackgroundLayer::SizeMode::Contain;
    else {
      const auto size = splitTopLevelSpaces(sizeValue);
      if (!size.isEmpty()) layer.sizeX = parseCssLengthPercentage(QStringView(size[0]), paintContext);
      if (size.size() > 1) layer.sizeY = parseCssLengthPercentage(QStringView(size[1]), paintContext);
    }
    const auto repeat = splitTopLevelSpaces(layerValue("background-repeat", index).toLower());
    if (!repeat.isEmpty()) {
      const auto mode = [](const QString& value) {
        using Repeat = ThemeBackgroundLayer::Repeat;
        return value == "no-repeat" ? Repeat::NoRepeat : value == "space" ? Repeat::Space : value == "round" ? Repeat::Round : Repeat::Repeat;
      };
      layer.repeatX = repeat[0] == "repeat-y" ? ThemeBackgroundLayer::Repeat::NoRepeat : mode(repeat[0]);
      layer.repeatY = repeat[0] == "repeat-x" ? ThemeBackgroundLayer::Repeat::NoRepeat : mode(repeat.size() > 1 ? repeat[1] : repeat[0]);
    }
    layer.blendMode = parseCssBlendMode(layerValue("background-blend-mode", index)).value_or(CssBlendMode::Normal);
    const auto origin = layerValue("background-origin", index), clip = layerValue("background-clip", index);
    if (!origin.isEmpty()) layer.origin = origin;
    if (!clip.isEmpty()) layer.clip = clip;
    layers.push_back(std::move(layer));
  }
  return layers;
}

ThemeElementStyle makeElementStyleForComputed(const QString& key, const CssComputedStyle& style) {
  const qreal emPx = style.fontSizePx * style.textScale;
  const qreal bodyPx = style.rootFontSizePx * style.textScale;
  static const std::vector<QString> colorProps = {QStringLiteral("color")};
  static const std::vector<QString> bgProps = {QStringLiteral("background-color"), QStringLiteral("background")};
  const auto styleColor = [&](const std::vector<QString>& properties) {
    for (const QString& property : properties) {
      auto raw = style.resolvedValue(property);
      if (raw.compare(QStringLiteral("currentColor"), Qt::CaseInsensitive) == 0 && property != QStringLiteral("color"))
        raw = style.resolvedValue(QStringLiteral("color"));
      const QColor c = extractColor(raw, style.customProperties());
      if (c.isValid()) { return c; }
    }
    return QColor();
  };
  const auto styleBox = [&](const QString& base) {
    ThemeElementBoxStyle box;
    const auto applySide = [&](const QString& side, auto setter) {
      const QString raw = style.rawValue(base + QLatin1Char('-') + side);
      if (raw.isEmpty()) { return; }
      qreal v = lengthToPx(raw, style.customProperties(), emPx, bodyPx, style.containingWidthPx);
      if (base == QStringLiteral("padding")) v = qMax<qreal>(0.0, v);
      setter(v);
      box.present = true;
    };
    if (style.hasProperty(base)) {
      const QMarginsF m = boxToMarginsPx(style.rawValue(base), style.customProperties(), emPx, bodyPx, style.containingWidthPx);
      if (base == QStringLiteral("margin")) { box.margin = m; } else { box.padding = m; }
      box.present = true;
    }
    QMarginsF& target = base == QStringLiteral("margin") ? box.margin : box.padding;
    applySide(QStringLiteral("top"), [&](qreal v) { target.setTop(v); });
    applySide(QStringLiteral("right"), [&](qreal v) { target.setRight(v); });
    applySide(QStringLiteral("bottom"), [&](qreal v) { target.setBottom(v); });
    applySide(QStringLiteral("left"), [&](qreal v) { target.setLeft(v); });
    return box;
  };
  ThemeElementStyle out;
  out.fingerprint = style.fingerprint();
  out.key = key;
  out.layout = CssLayoutStyle::fromComputed(style);
  out.box = styleBox(QStringLiteral("margin"));
  out.box.marginSpecified = out.box.present;
  const ThemeElementBoxStyle pad = styleBox(QStringLiteral("padding"));
  out.box.padding = pad.padding;
  out.box.paddingSpecified = pad.present;
  out.box.present = out.box.present || pad.present;
  const QStringList sides{QStringLiteral("top"), QStringLiteral("right"), QStringLiteral("bottom"), QStringLiteral("left")};
  for (int i = 0; i < 4; ++i) {
    out.box.marginLengths.sides[i] = style.length(QStringLiteral("margin-") + sides[i]);
    out.box.paddingLengths.sides[i] = style.length(QStringLiteral("padding-") + sides[i]);
  }
  out.box.margin = out.box.marginLengths.used(out.box.margin, style.containingWidthPx);
  out.box.padding = out.box.paddingLengths.used(out.box.padding, style.containingWidthPx, true);
  out.box.borderBox = style.resolvedValue(QStringLiteral("box-sizing")) == QStringLiteral("border-box");
  out.box.widthLength = style.length(QStringLiteral("width"));
  out.box.minWidthLength = style.length(QStringLiteral("min-width"));
  out.box.maxWidthLength = style.length(QStringLiteral("max-width"));
  out.box.heightLength = style.length(QStringLiteral("height"));
  out.box.minHeightLength = style.length(QStringLiteral("min-height"));
  out.box.maxHeightLength = style.length(QStringLiteral("max-height"));
  out.box.marginLeftAuto = style.resolvedValue(QStringLiteral("margin-left")) == QStringLiteral("auto");
  out.box.marginRightAuto = style.resolvedValue(QStringLiteral("margin-right")) == QStringLiteral("auto");
  const auto borderSide = [&](const QString& side, auto setW, auto setC) {
    const QString sh = style.rawValue(QStringLiteral("border-") + side);
    const QString wLong = style.rawValue(QStringLiteral("border-") + side + QStringLiteral("-width"));
    const QString cLong = style.rawValue(QStringLiteral("border-") + side + QStringLiteral("-color"));
    const QString globalSh = style.rawValue(QStringLiteral("border"));
    const QString globalW = style.rawValue(QStringLiteral("border-width"));
    const QString globalC = style.rawValue(QStringLiteral("border-color"));
    const QString borderStyle = style.resolvedValue(QStringLiteral("border-") + side + QStringLiteral("-style")).trimmed().toLower();
    if (borderStyle.isEmpty() || borderStyle == QStringLiteral("none") || borderStyle == QStringLiteral("hidden")) {
      setW(0.0);
      return;
    }
    const QString wRaw =
        !wLong.isEmpty()
            ? wLong
            : (!sh.isEmpty() ? sh : (!globalW.isEmpty() ? globalW : (!globalSh.isEmpty() ? globalSh : QStringLiteral("medium"))));
    const QString cRaw =
        !cLong.isEmpty()
            ? cLong
            : (!sh.isEmpty() ? sh : (!globalC.isEmpty() ? globalC : (!globalSh.isEmpty() ? globalSh : QStringLiteral("currentColor"))));
    qreal w = wRaw == QStringLiteral("thin")     ? 1.0
              : wRaw == QStringLiteral("medium") ? 3.0
              : wRaw == QStringLiteral("thick")  ? 5.0
                                                 : borderWidthPx(wRaw, style.customProperties(), emPx);
    const auto computedWidth = style.length(QStringLiteral("border-") + side + QStringLiteral("-width"));
    if (computedWidth.status == CssLengthStatus::Valid) w = qMax<qreal>(0, computedWidth.px);
    if (borderStyle == QStringLiteral("none") || borderStyle == QStringLiteral("hidden")) w = 0.0;
    setW(w);
    const QColor c = cRaw.compare(QStringLiteral("currentColor"), Qt::CaseInsensitive) == 0 ? styleColor(colorProps)
                                                                                            : extractColor(cRaw, style.customProperties());
    if (c.isValid()) {
      setC(c);
      out.box.present = true;
    }
  };
  borderSide(QStringLiteral("top"),    [&](qreal v) { out.box.borderTopWidth = v; },    [&](const QColor& v) { out.box.borderTopColor = v; });
  borderSide(QStringLiteral("right"),  [&](qreal v) { out.box.borderRightWidth = v; },  [&](const QColor& v) { out.box.borderRightColor = v; });
  borderSide(QStringLiteral("bottom"), [&](qreal v) { out.box.borderBottomWidth = v; }, [&](const QColor& v) { out.box.borderBottomColor = v; });
  borderSide(QStringLiteral("left"),   [&](qreal v) { out.box.borderLeftWidth = v; },   [&](const QColor& v) { out.box.borderLeftColor = v; });
  if (const qreal radius = lengthToPx(style.rawValue(QStringLiteral("border-radius")), style.customProperties(), emPx); radius > 0.0) {
    out.box.borderRadius = radius;
    out.box.present = true;
  }
  const QString widthRaw = style.resolvedValue(QStringLiteral("width")).trimmed().toLower();
  if (widthRaw == QStringLiteral("fit-content")) {
    out.box.widthFitContent = true;
    out.box.present = true;
  }
  out.paint.color = styleColor(colorProps);
  out.paint.backgroundColor = styleColor(bgProps);
  const CssLengthContext paintContext{emPx, bodyPx, emPx * .5, emPx * .5, style.viewportPx};
  out.paint.backgroundLayers=projectImageLayers(style,paintContext,[&](const QString& property){return style.resolvedValue(property);});
  out.paint.backgroundColorClip=out.paint.backgroundLayers.back().clip;
  const auto maskImages=CssThemeParser::splitTopLevelCommas(style.resolvedValue("mask-image"));
  if(std::any_of(maskImages.begin(),maskImages.end(),[](const auto& image){return image.trimmed()!="none";})) {
    const auto layers=projectImageLayers(style,paintContext,[&](QString property) {
      property.replace("background-","mask-");
      if(property=="mask-blend-mode")return QString("normal");
      if(property=="mask-position-x" || property=="mask-position-y")return QString();
      const auto value=style.resolvedValue(property);
      return value.isEmpty() && property=="mask-origin" ? QString("border-box") : value;
    });
    const auto modes=CssThemeParser::splitTopLevelCommas(style.resolvedValue("mask-mode"));
    const auto composites=CssThemeParser::splitTopLevelCommas(style.resolvedValue("mask-composite"));
    for(size_t i=0;i<layers.size();++i) {
      ThemeMaskLayer layer;layer.image=layers[i];
      layer.none=maskImages[i%maskImages.size()].trimmed()=="none";
      layer.luminance=!modes.isEmpty() && modes[i%modes.size()].trimmed()=="luminance";
      if(!composites.isEmpty())layer.composite=composites[i%composites.size()].trimmed();
      out.paint.maskLayers.push_back(std::move(layer));
    }
  }
  out.cloneInlineDecoration = style.resolvedValue("box-decoration-break") == "clone" ||
                              style.resolvedValue("-webkit-box-decoration-break") == "clone";
  bool opacityOk = false;
  auto opacity = style.resolvedValue("opacity");
  const bool opacityPercent = opacity.endsWith('%');
  if (opacityPercent) opacity.chop(1);
  const auto alpha = opacity.toDouble(&opacityOk);
  if (opacityOk) out.paint.opacity = qBound<qreal>(0, opacityPercent ? alpha / 100 : alpha, 1);
  const QString shadow = style.rawValue(QStringLiteral("box-shadow"));
  if (!shadow.isEmpty() && !shadow.contains(QStringLiteral("none"))) {
    const auto parsed = parseFirstBoxShadow(shadow, style.customProperties(), emPx);
    out.paint.boxShadowColor = parsed.color;
    out.paint.boxShadowBlur = parsed.blur;
    out.paint.boxShadowOffsetX = parsed.offsetX;
    out.paint.boxShadowOffsetY = parsed.offsetY;
    out.paint.boxShadowSpread = parsed.spread;
  }
  const QString transform = style.resolvedValue(QStringLiteral("transform")).trimmed().toLower();
  static const QRegularExpression transformRe(QStringLiteral(R"((translate[xy]?|scale[xy]?|rotate)\(([^()]*(?:\([^()]*\)[^()]*)*)\))"));
  auto transforms = transformRe.globalMatch(transform);
  while (transforms.hasNext()) {
    const auto m = transforms.next();
    auto args = m.captured(2); args.replace(',', ' ');
    const auto parts = splitTopLevelSpaces(args);
    if (parts.isEmpty()) continue;
    ThemeElementPaintStyle::Transform t;
    const auto name = m.captured(1);
    if (name.startsWith("translate")) {
      t.x = parseCssLengthPercentage(QStringView(parts[0]), paintContext);
      if (name == "translatey") { t.y = t.x; t.x = {}; }
      else if (parts.size() > 1) t.y = parseCssLengthPercentage(QStringView(parts[1]), paintContext);
    } else if (name.startsWith("scale")) {
      t.kind = ThemeElementPaintStyle::Transform::Kind::Scale;
      bool ok = false; const qreal x = parts[0].toDouble(&ok);
      if (!ok) continue;
      t.x = {CssLengthStatus::Valid, name == "scaley" ? 1 : x};
      t.y = {CssLengthStatus::Valid, name == "scalex" ? 1 : parts.size() > 1 ? parts[1].toDouble() : x};
      if (name == "scale") out.paint.transformScale = x;  // compatibility projection
    } else {
      t.kind = ThemeElementPaintStyle::Transform::Kind::Rotate;
      auto angle = parts[0]; qreal factor = 1;
      if (angle.endsWith("turn")) { angle.chop(4); factor = 360; }
      else if (angle.endsWith("grad")) { angle.chop(4); factor = .9; }
      else if (angle.endsWith("rad")) { angle.chop(3); factor = 180 / M_PI; }
      else if (angle.endsWith("deg")) angle.chop(3);
      t.angle = angle.toDouble() * factor;
    }
    out.paint.transforms.push_back(t);
  }
  // Preserve authored function order and repetitions. Legacy scalar views
  // remain available to existing callers, but painting uses the ordered lists.
  out.paint.filters = parseCssFilters(style.resolvedValue("filter"),paintContext,out.paint.color).value_or(std::vector<CssFilterOperation>{});
  out.paint.backdropFilters = parseCssFilters(style.resolvedValue("backdrop-filter"),paintContext,out.paint.color).value_or(std::vector<CssFilterOperation>{});
  const auto scalarViews = [](const auto& list, qreal& blur, qreal& brightness, qreal& contrast, qreal& grayscale,
                              qreal& sepia, qreal& hue, qreal& opacity) {
    for(const auto& op:list) switch(op.kind) {
      case CssFilterOperation::Kind::Blur: blur=op.value;break;
      case CssFilterOperation::Kind::Brightness: brightness=op.value;break;
      case CssFilterOperation::Kind::Contrast: contrast=op.value;break;
      case CssFilterOperation::Kind::Grayscale: grayscale=op.value;break;
      case CssFilterOperation::Kind::Sepia: sepia=op.value;break;
      case CssFilterOperation::Kind::HueRotate: hue=op.value;break;
      case CssFilterOperation::Kind::Opacity: opacity=op.value;break;
      default:break;
    }
  };
  auto& paint=out.paint;
  paint.filterPresent=!paint.filters.empty();paint.backdropPresent=!paint.backdropFilters.empty();
  scalarViews(paint.filters,paint.filterBlur,paint.filterBrightness,paint.filterContrast,paint.filterGrayscale,
      paint.filterSepia,paint.filterHueRotateDeg,paint.filterOpacity);
  scalarViews(paint.backdropFilters,paint.backdropBlur,paint.backdropBrightness,paint.backdropContrast,paint.backdropGrayscale,
      paint.backdropSepia,paint.backdropHueRotateDeg,paint.backdropOpacity);
  out.text.fontFamily = firstFamily(style.rawValue(QStringLiteral("font-family")), style.customProperties());
  out.text.fontSizePx = style.hasProperty(QStringLiteral("font-size")) ? style.fontSizePx : 0.0;
  out.text.fontSizeSet = style.hasProperty(QStringLiteral("font-size"));
  out.text.lineHeight =
      parseLineHeightMultiplier(style.rawValue(QStringLiteral("line-height")), style.customProperties(), style.fontSizePx);
  out.text.wordSpacing = style.length(QStringLiteral("word-spacing")).px;
  out.box.borderTopStyle = style.resolvedValue(QStringLiteral("border-top-style"));
  out.box.borderRightStyle = style.resolvedValue(QStringLiteral("border-right-style"));
  out.box.borderBottomStyle = style.resolvedValue(QStringLiteral("border-bottom-style"));
  out.box.borderLeftStyle = style.resolvedValue(QStringLiteral("border-left-style"));
  out.text.letterSpacing = style.length(QStringLiteral("letter-spacing")).px;
  const auto lines = style.resolvedValue(QStringLiteral("text-decoration-line"));
  if (lines.contains(QStringLiteral("underline"))) out.text.decorationLines |= 1;
  if (lines.contains(QStringLiteral("overline"))) out.text.decorationLines |= 2;
  if (lines.contains(QStringLiteral("line-through"))) out.text.decorationLines |= 4;
  const auto lineStyle = style.resolvedValue(QStringLiteral("text-decoration-style"));
  out.text.underlineStyle = lineStyle == "dotted"   ? QTextCharFormat::DotLine
                            : lineStyle == "dashed" ? QTextCharFormat::DashUnderline
                            : lineStyle == "wavy"   ? QTextCharFormat::WaveUnderline
                                                    : QTextCharFormat::SingleUnderline;
  out.text.decorationColor = extractColor(style.resolvedValue(QStringLiteral("text-decoration-color")), style.customProperties());

  out.text.alignment = parseTextAlign(style.rawValue(QStringLiteral("text-align")), style.customProperties());
  const QString ttRaw = style.resolvedValue(QStringLiteral("text-transform")).trimmed().toLower();
  if (ttRaw == QStringLiteral("uppercase")) { out.text.textTransform = 1; }
  else if (ttRaw == QStringLiteral("lowercase")) { out.text.textTransform = 2; }
  else if (ttRaw == QStringLiteral("capitalize")) { out.text.textTransform = 3; }
  // CSS `text-shadow: <ox> <oy> <blur>? <color>` (first of a comma list).
  const QString tsRaw = style.resolvedValue(QStringLiteral("text-shadow")).trimmed();
  if (!tsRaw.isEmpty() && !tsRaw.startsWith(QStringLiteral("none"))) {
    const QString first = CssThemeParser::splitTopLevelCommas(tsRaw).first().trimmed();
    const QColor sc = extractColor(first, style.customProperties());
    if (sc.isValid()) {
      qreal nums[3] = {0, 0, 0};
      int count = 0;
      for (const auto& token : splitTopLevelSpaces(first)) {
        const auto length = parseCssLengthPercentage(QStringView(token), paintContext);
        if (length.status == CssLengthStatus::Valid && !length.hasPercentage && count < 3) nums[count++] = length.px;
      }
      out.text.textShadow.offset = QPointF(nums[0], nums[1]);
      out.text.textShadow.blur = count >= 3 ? nums[2] : 0.0;
      out.text.textShadow.color = sc;
      out.text.textShadow.present = true;
    }
  }
  const auto stroke = splitTopLevelSpaces(style.resolvedValue("-webkit-text-stroke"));
  out.text.strokeColor = out.paint.color;
  for (const auto& token : stroke) {
    const auto length = parseCssLengthPercentage(QStringView(token), paintContext);
    if (length.status == CssLengthStatus::Valid) out.text.strokeWidth = qMax<qreal>(0, length.px);
    else if (const auto color = extractColor(token, style.customProperties()); color.isValid()) out.text.strokeColor = color;
  }
  if (style.hasProperty("-webkit-text-stroke-width")) out.text.strokeWidth = qMax<qreal>(0, style.length("-webkit-text-stroke-width").px);
  if (style.hasProperty("-webkit-text-stroke-color")) out.text.strokeColor = extractColor(style.resolvedValue("-webkit-text-stroke-color"), style.customProperties());
  const ParsedFontWeight fw = parseFontWeight(style.rawValue(QStringLiteral("font-weight")), style.customProperties());
  if (fw.present) { out.text.fontWeight = fw.weight; out.text.fontWeightSet = true; }
  const ParsedItalic fs = parseFontItalic(style.rawValue(QStringLiteral("font-style")), style.customProperties());
  if (fs.present) { out.text.italic = fs.italic; out.text.italicSet = true; }
  return out;
}

ThemeDefinition CssThemeMapper::fromSheet(const CssThemeSheet& source, const QString& id, CssEnvironment environment,
                                          const CssComputedStyleEngine* computedEngine) {
  diag::ScopedPerfProbe perf("theme.map", mapperLoadPerf());
  const CssThemeSheet sheet = documentStyleSheet(source).evaluated(environment);
  ThemeDefinition d;
  d.isBuiltIn = false;
  d.id = id.toLower();
  d.sourceSheet = std::make_shared<CssThemeSheet>(source);
  const auto ownedEngine =
      computedEngine ? nullptr : std::make_shared<CssComputedStyleEngine>(documentStyleSheet(source), environment);
  const auto& engine = computedEngine ? *computedEngine : *ownedEngine;
  DocumentStylePrototypes tree(true);
  struct PrototypeCacheScope {
    const CssComputedStyleEngine& engine;
    const DocumentStylePrototypes& tree;
    ~PrototypeCacheScope() { engine.forgetElements(tree.cacheIdentities()); }
  } prototypeCacheScope{engine, tree};
  ComputedDecorationStyles styles;
  const QStringList hosts{"html",  "body",     "#write",     "p",     "blockquote", "blockquote p", "ul",
                          "ol",    "li",       "li::marker", "pre",   ".md-fences", "code",         "pre code",
                          "kbd",   "a",        "mark",       "del",   "em",         "hr",           "table",
                          "thead", "thead tr", "th",         "tbody", "tbody tr",   "td",           ".MathJax",
                          "h1",    "h2",       "h3",         "h4",    "h5",         "h6",           ".table-figure"};
  {
    diag::ScopedPerfProbe stylesPerf("theme.mapStyles", mapperLoadPerf());
    for (const auto& host : hosts) {
      const auto& element = tree.element(host);
      const auto ordinary = engine.styleFor(element);
      styles.insert(host, ordinary);
      const auto stateMask = engine.interactiveStateMask(element);
      CssElementState hover;
      hover.hover = true;
      CssElementState focus;
      focus.focus = true;
      styles.insert(host + QStringLiteral(":hover"), stateMask & 1 ? engine.styleFor(element, hover) : ordinary);
      styles.insert(host + QStringLiteral(":focus"), stateMask & 2 ? engine.styleFor(element, focus) : ordinary);
      for (const auto& pseudo : {QStringLiteral("before"), QStringLiteral("after"), QStringLiteral("selection")}) {
        const auto key = host + QStringLiteral("::") + pseudo;
        const auto& el = tree.element(key);
        const auto normal = engine.styleFor(el);
        const auto mask = engine.interactiveStateMask(el);
        styles.insert(key, normal);
        styles.insert(key + QStringLiteral(":hover"), mask & 1 ? engine.styleFor(el, hover) : normal);
        styles.insert(key + QStringLiteral(":focus"), mask & 2 ? engine.styleFor(el, focus) : normal);
      }
    }
  }
  diag::ScopedPerfProbe projectionPerf("theme.mapProjection", mapperLoadPerf());
  const auto style = [&](const QString& key) -> const CssComputedStyle& { return styles.constFind(key).value(); };
  const auto project = [&](const QString& key) { return projectComputedStyle(key, style(key)); };
  const auto color = [&](const QString& key, const char* property = "color") {
    return extractColor(style(key).resolvedValue(QString::fromLatin1(property)), style(key).customProperties());
  };
  const auto vars = style(QStringLiteral("html")).customProperties();
  ThemeColors& k = d.colors;
  d.bodyFontPx = style(QStringLiteral("#write")).fontSizePx;
  for (const auto& host : hosts) {
    d.elementStyles.push_back(project(host));
    d.elementStyles.push_back(project(host + QStringLiteral(":hover")));
    d.elementStyles.push_back(project(host + QStringLiteral(":focus")));
  }
  d.page.viewportBackground = color(QStringLiteral("body"), "background-color");
  if (!d.page.viewportBackground.isValid()) d.page.viewportBackground = color(QStringLiteral("html"), "background-color");
  d.page.pageBackground = color(QStringLiteral("#write"), "background-color");
  k.background = d.page.pageBackground.isValid() ? d.page.pageBackground : d.page.viewportBackground;
  k.text = color(QStringLiteral("p"));
  k.link = color(QStringLiteral("a"));
  k.codeBackground = color(QStringLiteral("code"), "background-color");
  if (k.codeBackground.alpha() == 0) k.codeBackground = {};
  k.codeBlockBackground = color(QStringLiteral("pre"), "background-color");
  k.highlight = color(QStringLiteral("mark"), "background-color");
  k.selection = color(QStringLiteral("#write::selection"), "background-color");
  k.blockquoteBackground = color(QStringLiteral("blockquote"), "background-color");
  k.quoteBorder = project(QStringLiteral("blockquote")).box.borderLeftColor;
  k.tableBorder = project(QStringLiteral("td")).box.borderTopColor;
  if (!k.tableBorder.isValid()) k.tableBorder = project(QStringLiteral("table")).box.borderTopColor;
  k.tableHeaderBackground = color(QStringLiteral("th"), "background-color");
  if (!k.tableHeaderBackground.isValid()) k.tableHeaderBackground = color(QStringLiteral("thead"), "background-color");
  CssElement even = tree.element(QStringLiteral("tbody tr"));
  even.cacheId = 0;  // Its changed sibling position is not the cached prototype.
  even.childIndex = 1;
  const auto evenStyle = engine.styleFor(even);
  k.tableAlternateBackground = extractColor(evenStyle.resolvedValue(QStringLiteral("background-color")), evenStyle.customProperties());
  // Palette tokens describe visible surfaces; currentColor on a zero-width
  // border must not suppress the chrome's soft-edge fallback.
  const auto codeBox = project(QStringLiteral("code")).box;
  const auto preBox = project(QStringLiteral("pre")).box;
  if (codeBox.borderTopWidth > 0)
    k.codeBorder = codeBox.borderTopColor;
  else if (preBox.borderTopWidth > 0)
    k.codeBorder = preBox.borderTopColor;
  k.headingAccentColor = project(QStringLiteral("h2")).box.borderLeftColor;
  const auto documentBox =
      projectComputedStyle(QStringLiteral("#write"), style(QStringLiteral("#write")).withContainingWidth(environment.viewportWidth));
  d.page.pagePadding = documentBox.box.padding;
  d.page.pageMargin = documentBox.box.margin;
  d.page.pageMarginExplicit = documentBox.box.present;
  d.page.borderBox = documentBox.box.borderBox;
  d.page.pageBorderWidth = documentBox.box.borderTopWidth;
  d.page.pageBorderColor = documentBox.box.borderTopColor;
  d.page.pageBorderRadius = documentBox.box.borderRadius;
  const auto pageLength = [&](const QString& property) {
    const auto& computed = style(QStringLiteral("#write"));
    const QString value = computed.resolvedValue(property);
    if (isIntrinsicPageWidthKeyword(value)) return kUnboundedPageWidth;
    const auto length = computed.length(property);
    return length.status == CssLengthStatus::Valid ? length.used(environment.viewportWidth) : 0.0;
  };
  const qreal preferred = pageLength(QStringLiteral("width")), maximum = pageLength(QStringLiteral("max-width"));
  d.page.pageMaxWidth = preferred > 0 ? preferred : maximum;
  if (preferred > 0 && maximum > 0) d.page.pageMaxWidth = qMin(preferred, maximum);
  const auto shadow = parseFirstBoxShadow(style(QStringLiteral("#write")).resolvedValue(QStringLiteral("box-shadow")), vars, d.bodyFontPx);
  if (shadow.present) {
    d.page.pageShadowColor = shadow.color;
    d.page.pageShadowOffsetX = shadow.offsetX;
    d.page.pageShadowOffsetY = shadow.offsetY;
    d.page.pageShadowBlur = shadow.blur;
    d.page.pageShadowSpread = shadow.spread;
  }
  auto& ty = d.typography;
  const auto p = project(QStringLiteral("p"));
  const auto code = project(QStringLiteral("code"));
  const auto pre = project(QStringLiteral("pre"));
  const auto kbd = project(QStringLiteral("kbd"));
  ty.bodyFont = p.text.fontFamily;
  ty.bodySizePt = style(QStringLiteral("#write")).hasProperty(QStringLiteral("font-size")) ? pxToPt(d.bodyFontPx) : 0;
  ty.headingFont = project(QStringLiteral("h1")).text.fontFamily;
  ty.codeFont = code.text.fontFamily;
  ty.mathFont = project(QStringLiteral(".MathJax")).text.fontFamily;
  if (style(QStringLiteral(".MathJax")).hasProperty(QStringLiteral("font-size")))
    ty.mathSizePt = pxToPt(style(QStringLiteral(".MathJax")).fontSizePx);
  ty.bodyAlignment = p.text.alignment;
  ty.lineHeight = p.text.lineHeight;
  ty.letterSpacing = style(QStringLiteral("p")).length(QStringLiteral("letter-spacing")).px;
  ty.codeLetterSpacing = style(QStringLiteral("code")).length(QStringLiteral("letter-spacing")).px;
  k.serifBody = fontStackLooksSerif(ty.bodyFont) || fontStackLooksSerif(ty.headingFont);
  ty.inlineCodeTextColor = code.paint.color;
  ty.delColor = color(QStringLiteral("del"));
  if (code.box.paddingSpecified) {
    ty.inlineCodePaddingH = qMax(code.box.padding.left(), code.box.padding.right());
    ty.inlineCodePaddingV = qMax(code.box.padding.top(), code.box.padding.bottom());
  }
  ty.inlineCodeBorderWidth = code.box.borderTopWidth;
  if (style(QStringLiteral("code")).hasProperty(QStringLiteral("border-radius"))) ty.inlineCodeBorderRadius = code.box.borderRadius;
  const auto codeShadow = parseFirstBoxShadow(style(QStringLiteral("code")).resolvedValue(QStringLiteral("box-shadow")), vars,
                                              style(QStringLiteral("code")).fontSizePx);
  if (codeShadow.present) {
    ty.inlineCodeShadowColor = codeShadow.color;
    ty.inlineCodeShadowOffsetX = codeShadow.offsetX;
    ty.inlineCodeShadowOffsetY = codeShadow.offsetY;
    ty.inlineCodeShadowBlur = codeShadow.blur;
    ty.inlineCodeShadowSpread = codeShadow.spread;
  }
  ty.kbdTextColor = kbd.paint.color;
  ty.kbdBackground = kbd.paint.backgroundColor;
  ty.kbdFont = kbd.text.fontFamily;
  ty.kbdPaddingH = qMax(kbd.box.padding.left(), kbd.box.padding.right());
  ty.kbdPaddingV = qMax(kbd.box.padding.top(), kbd.box.padding.bottom());
  ty.kbdBorderWidth = kbd.box.borderTopWidth;
  ty.kbdBorderColor = kbd.box.borderTopColor;
  ty.kbdBorderRadius = kbd.box.borderRadius;
  ty.kbdBorderBottomWidth = kbd.box.borderBottomWidth;
  ty.kbdBorderBottomColor = kbd.box.borderBottomColor;
  ty.kbdShadowColor = kbd.paint.boxShadowColor;
  d.spacing.codeBlockMargin = pre.box.margin;
  d.spacing.codeBlockPadding = pre.box.padding;
  d.spacing.codeBlockBorderRadius = pre.box.borderRadius;
  d.spacing.codeBlockBoxThemed = pre.box.present;
  const auto table = project(QStringLiteral("table"));
  const auto td = project(QStringLiteral("td"));
  const auto th = project(QStringLiteral("th"));
  d.spacing.tableMargin = table.box.margin;
  d.spacing.tableCellPadding = td.box.paddingSpecified ? td.box.padding : th.box.padding;
  d.spacing.tableBorderRadius = table.box.borderRadius;
  d.spacing.tableBoxThemed = td.box.present || th.box.present;
  d.spacing.listMargin = project(QStringLiteral("ul")).box.margin;
  const auto listType = [&](const QString& host) {
    const auto& st = style(host);
    QString type = st.resolvedValue(QStringLiteral("list-style-type"));
    if (type.isEmpty()) type = st.resolvedValue(QStringLiteral("list-style")).section(QLatin1Char(' '), 0, 0);
    return type;
  };
  d.spacing.ulListStyleType = listType(QStringLiteral("ul"));
  d.spacing.olListStyleType = listType(QStringLiteral("ol"));
  d.spacing.liListStyleType = listType(QStringLiteral("li"));
  d.decorations.listMarkerContent = parseContentTokens(style(QStringLiteral("li::marker")).resolvedValue(QStringLiteral("content")));
  if (std::none_of(d.decorations.listMarkerContent.begin(), d.decorations.listMarkerContent.end(),
                   [](const ContentToken& token) { return token.kind != ContentToken::Kind::Literal; }))
    d.decorations.listMarkerContent.clear();
  const auto parseCounters = [](const QString& value, int step) {
    QVector<QPair<QString, int>> out;
    const auto parts = splitTopLevelSpaces(value);
    for (int i = 0; i < parts.size(); ++i) {
      if (parts[i] == QStringLiteral("none")) return QVector<QPair<QString, int>>{};
      const auto name = parts[i];
      int amount = step;
      if (i + 1 < parts.size()) {
        bool ok = false;
        const int v = parts[i + 1].toInt(&ok);
        if (ok) {
          amount = v;
          ++i;
        }
      }
      out.append({name, amount});
    }
    return out;
  };
  d.decorations.backgrounds = extractElementBackgrounds(styles);
  d.decorations.hoverEffects = extractHoverEffects(styles);
  d.decorations.transitions = extractTransitions(styles);
  d.decorations.keyframes = extractKeyframes(sheet, vars);
  d.decorations.animations = extractAnimations(styles);
  d.decorations.pseudos = extractPseudoRules(styles);
  for (int level = 1; level <= 6; ++level) {
    const QString host = QStringLiteral("h%1").arg(level);
    const auto h = project(host);
    ty.headingColor[level - 1] = h.paint.color;
    ty.headingSizePt[level - 1] = pxToPt(h.text.fontSizePx);
    ty.headingAlignment[level - 1] = h.text.alignment;
    ty.headingLineHeight[level - 1] = h.text.lineHeight;
    ty.headingFontWeight[level - 1] = h.text.fontWeight;
    ty.headingFontWeightSet[level - 1] = h.text.fontWeightSet;
    ty.headingItalic[level - 1] = h.text.italic;
    ty.headingItalicSet[level - 1] = h.text.italicSet;
    for (const auto& key : {host, host + QStringLiteral("::before"), host + QStringLiteral("::after")}) {
      ThemeDecorations::CounterOps ops;
      ops.resets = parseCounters(style(key).resolvedValue(QStringLiteral("counter-reset")), 0);
      ops.increments = parseCounters(style(key).resolvedValue(QStringLiteral("counter-increment")), 1);
      if (!ops.resets.isEmpty() || !ops.increments.isEmpty()) d.decorations.hostCounterOps.insert(key, ops);
    }
    for (const auto& rule : d.decorations.pseudos) {
      if (rule.host != host) continue;
      if (std::any_of(rule.contentTokens.begin(), rule.contentTokens.end(),
                      [](const ContentToken& t) { return t.kind == ContentToken::Kind::Counter || t.kind == ContentToken::Kind::Counters; }))
        d.decorations.hasHeadingCounters = true;
    }
  }
  ThemeDecorations::CounterOps rootOps;
  for (const auto& key : {QStringLiteral("html"), QStringLiteral("body"), QStringLiteral("#write")})
    rootOps.resets += parseCounters(style(key).resolvedValue(QStringLiteral("counter-reset")), 0);
  if (!rootOps.resets.isEmpty()) d.decorations.hostCounterOps.insert(QStringLiteral("#write"), rootOps);
  const auto link = project(QStringLiteral("a")).text;
  ty.linkUnderlined = link.decorationLines & 1;
  ty.linkOverline = link.decorationLines & 2;
  ty.linkUnderlineStyle = link.underlineStyle;
  ty.linkUnderlineColor = link.decorationColor;
  // --- Tier 2: conventional :root variable vocabulary (gap fill) ----------
  // CSS themes speak a conventional :root vocabulary (--bg-color,
  // --text-color, --primary-color, --side-bar-bg-color, …). That vocabulary IS
  // the author's semantic declaration of "this is my background / text / accent",
  // so it is strictly more reliable than guessing from selector names. But a
  // concrete element rule from tier 1 is authoritative for what is actually
  // painted (a theme may set body { background:#fff } yet keep --bg-color for a
  // different purpose) — so these variables only fill tokens the cascade LEFT
  // INVALID, never overwrite. This is what makes "pure variable" themes (only
  // :root + an @imported base that applies the colours, e.g. the a community theme (phycat)
  // family) resolve fully without chasing selector names forever.
  {
    // Resolve a variable's value to a colour, resolving nested var() and
    // shorthands (extractColor handles both). Tries names in order.
    const auto firstVar = [&](std::initializer_list<const char*> names) -> QColor {
      for (const char* n : names) {
        const QString raw = vars.value(QString::fromLatin1(n));
        if (raw.isEmpty()) { continue; }
        const QColor c = extractColor(raw, vars);
        if (c.isValid()) { return c; }
      }
      return QColor();
    };
    // Set `member` from the first resolving variable, but only if still unset.
    const auto gap = [&](QColor ThemeColors::* member, std::initializer_list<const char*> names) {
      if ((k.*member).isValid()) { return; }
      const QColor c = firstVar(names);
      if (c.isValid()) { k.*member = c; }
    };

    // Document palette. Background also paints the viewport when no
    // html/body/#write rule did, so the page card model stays consistent.
    if (!k.background.isValid()) {
      const QColor bg = firstVar({"--bg-color"});
      if (bg.isValid()) {
        k.background = bg;
        if (!d.page.viewportBackground.isValid()) { d.page.viewportBackground = bg; }
      }
    }
    gap(&ThemeColors::text,                {"--text-color"});
    gap(&ThemeColors::muted,               {"--text-color-secondary", "--text-color-tertiary"});
    gap(&ThemeColors::link,                {"--primary-color"});
    gap(&ThemeColors::codeBackground,      {"--code-bg-color"});
    gap(&ThemeColors::codeBlockBackground, {"--code-block-bg"});
    gap(&ThemeColors::selection,           {"--select-text-bg-color"});
    gap(&ThemeColors::accent,              {"--primary-color"});
    // Chrome palette — the conventional UI-control vocabulary maps directly onto Muffin's
    // chrome tokens, so a stock CSS theme lights up the whole chrome for free
    // (previously only reachable via --muffin-*).
    gap(&ThemeColors::chromeBackground,    {"--side-bar-bg-color"});
    gap(&ThemeColors::surface,             {"--side-bar-bg-color"});
    // Typora's --control-text-color is secondary UI ink (sidebar controls and
    // captions), not primary navigation text. Mapping it to chromeText made the
    // GitHub menu #777 instead of its primary #333. Leave chromeText unset so
    // deriveChromeDefaults takes the document's primary text; a theme that needs
    // a distinct primary chrome colour can declare --muffin-chrome-text.
    gap(&ThemeColors::chromeMuted,
        {"--control-text-color", "--text-color-secondary", "--text-color-tertiary"});
    gap(&ThemeColors::hover,               {"--item-hover-bg-color"});
    gap(&ThemeColors::selected,            {"--active-file-bg-color"});
  }

  // --- Tier 3: Muffin-specific extension variables (--muffin-*). Highest
  // authority — explicit Muffin intent wins over both cascade and the
  // conventional vocabulary.
  if (QColor cb = varColor(vars, "--muffin-background"); cb.isValid()) { k.background = cb; }
  if (QColor ct = varColor(vars, "--muffin-text"); ct.isValid()) { k.text = ct; }
  if (QColor cl = varColor(vars, "--muffin-link"); cl.isValid()) { k.link = cl; }
  if (QColor cc = varColor(vars, "--muffin-code-background"); cc.isValid()) { k.codeBackground = cc; }
  if (QColor ccb = varColor(vars, "--muffin-code-block-background"); ccb.isValid()) { k.codeBlockBackground = ccb; }
  if (QColor ch = varColor(vars, "--muffin-highlight"); ch.isValid()) { k.highlight = ch; }
  if (QColor cs = varColor(vars, "--muffin-selection"); cs.isValid()) { k.selection = cs; }
  if (QColor cq = varColor(vars, "--muffin-quote-border"); cq.isValid()) { k.quoteBorder = cq; }
  if (QColor cqb = varColor(vars, "--muffin-blockquote-background"); cqb.isValid()) { k.blockquoteBackground = cqb; }
  if (QColor ct2 = varColor(vars, "--muffin-table-border"); ct2.isValid()) { k.tableBorder = ct2; }
  if (QColor cth = varColor(vars, "--muffin-table-header-background"); cth.isValid()) { k.tableHeaderBackground = cth; }
  if (QColor cta = varColor(vars, "--muffin-table-alternate-background"); cta.isValid()) { k.tableAlternateBackground = cta; }
  if (QColor cbr = varColor(vars, "--muffin-code-border"); cbr.isValid()) { k.codeBorder = cbr; }
  if (QColor cha = varColor(vars, "--muffin-heading-accent"); cha.isValid()) { k.headingAccentColor = cha; }
  // Chrome palette — no CSS equivalent, so only via --muffin-*. These are
  // guarded (not bare assignment) so an absent --muffin-* leaves a tier-2 value
  // intact instead of clobbering it back to invalid.
  if (QColor cm = varColor(vars, "--muffin-muted"); cm.isValid()) { k.muted = cm; }
  if (QColor ccb = varColor(vars, "--muffin-chrome-background"); ccb.isValid()) { k.chromeBackground = ccb; }
  if (QColor cct = varColor(vars, "--muffin-chrome-text"); cct.isValid()) { k.chromeText = cct; }
  if (QColor ccm = varColor(vars, "--muffin-chrome-muted"); ccm.isValid()) { k.chromeMuted = ccm; }
  if (QColor cs = varColor(vars, "--muffin-surface"); cs.isValid()) { k.surface = cs; }
  if (QColor cc = varColor(vars, "--muffin-canvas"); cc.isValid()) { k.canvas = cc; }
  if (QColor cb = varColor(vars, "--muffin-border"); cb.isValid()) { k.border = cb; }
  if (QColor ch = varColor(vars, "--muffin-hover"); ch.isValid()) { k.hover = ch; }
  if (QColor cs2 = varColor(vars, "--muffin-selected"); cs2.isValid()) { k.selected = cs2; }
  if (QColor ca = varColor(vars, "--muffin-accent"); ca.isValid()) { k.accent = ca; }
  if (truthy(varValue(vars, "--muffin-serif-body"))) { k.serifBody = true; }

  // Complete either half of a one-sided document palette before deriving the
  // remaining chrome and muted tokens. For text-only themes, choose a canvas by
  // luminance so isDark and chrome derivation stay sane — mirrors a browser's
  // default white/dark page when CSS specifies none.
  // Typora themes may only paint #write and rely on the browser/host sheet's
  // inherited text colour. Muffin has no UA sheet beneath imported CSS, so
  // materialise a contrast-safe equivalent before validating the palette.
  if (!k.text.isValid() && k.background.isValid()) {
    k.text = typoraHostInk(k.background);
  }
  if (!k.background.isValid() && k.text.isValid()) {
    k.background = k.text.lightness() >= 128 ? QColor(0x18, 0x18, 0x18) : QColor(0xff, 0xff, 0xff);
    if (!d.page.viewportBackground.isValid()) { d.page.viewportBackground = k.background; }
  }
  // A #write-only background is a document card over Typora's host canvas, not
  // a request to paint the whole viewport the same colour. Supply the missing
  // host surface only when body/html did not explicitly paint one.
  if (!d.page.viewportBackground.isValid() && d.page.pageBackground.isValid()) {
    d.page.viewportBackground = relativeLuminance(d.page.pageBackground) > 0.179
        ? QColor(QStringLiteral("#f3f3f3"))
        : QColor(QStringLiteral("#202124"));
  }
  // Derive chrome defaults for anything still unset, then resolve isDark.
  ThemeDefinition::deriveChromeDefaults(k);
  // CSS themes have no "muted text" concept; ensure muted/chromeMuted resolve
  // to something valid (text colour, slightly softened) so chrome reads sanely.
  if (!k.muted.isValid()) {
    k.muted = k.text.isValid() ? (k.isDark ? k.text.darker(160) : k.text.lighter(160)) : k.text;
  }
  if (!k.chromeMuted.isValid()) { k.chromeMuted = k.muted; }
  const QString darkFlag = varValue(vars, "--muffin-dark");
  if (!darkFlag.isEmpty()) {
    k.isDark = truthy(darkFlag);
  } else {
    k.isDark = k.background.isValid() ? (k.background.lightness() < 128) : false;
  }

  // Label: explicit --muffin-label wins, else a presentable title-cased id.
  const QString labelVar = varValue(vars, "--muffin-label");
  if (!labelVar.isEmpty()) {
    d.label = labelVar;
    if (d.label.size() >= 2 && d.label.front() == QLatin1Char('"') && d.label.back() == QLatin1Char('"')) {
      d.label = d.label.mid(1, d.label.size() - 2);
    }
  } else {
    d.label = titleCaseId(d.id);
  }
  if (themeStyleLog().isDebugEnabled()) {
    qCDebug(themeStyleLog).noquote() << formatThemeDefinitionSummary(d);
  }
  d.hasStructuralRules = engine.selectorFeatures().hasStructuralRules;
  d.hasNthOfType = engine.selectorFeatures().needsTypeIndex;
  if (ownedEngine) {
    ownedEngine->clearCache();
    d.preparedEngine = ownedEngine;
    d.preparedSourceRevision = source.revision();
  }
  return d;
}

ThemeElementStyle CssThemeMapper::projectComputedStyle(const QString& key, const CssComputedStyle& style) {
  return makeElementStyleForComputed(key, style);
}

}  // namespace muffin

#include "theme/CssComputedStyleEngine.h"
#include "theme/CssThemeParser.h"

#include <QCoreApplication>
#include "../TestUtils.h"

using namespace muffin;

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const QHash<QString, QString> variables{{"--weight", "500"}, {"--number", "2"}, {"--unit", "px"}};
  require(CssThemeParser::resolveVars("var(--weight)1em/1.5 serif", variables) == "500 1em/1.5 serif",
          QStringLiteral("var substitution preserves token boundaries in font shorthand"));
  require(CssThemeParser::resolveVars("var(--number)var(--unit)", variables) == "2 px",
          QStringLiteral("adjacent variable tokens cannot create a dimension"));
  const QStringList supported = {
      "(display: grid)", "(color: #123456)", "(padding: 2px 3px)", "(backdrop-filter: blur(1px))",
      "(background: red)", "(background-image: linear-gradient(red, blue))",
      "(display:grid)and (color:red)",
      "(filter: blur(2px) brightness(80%))", "(filter: invert(.5) saturate(2) drop-shadow(2px 3px 1px red))", "(counter-set: heading 1)", "not (corner-shape: squircle)",
      "((display: grid) and (color: red)) or (unknown: value)",
      "not ((display: grid) and (unknown: value))", "(display: flex) AND /* comment */ (color: red)",
      "(--label: \"or ) and (\")", "(color: red !important)", "not selector(:unknown)"};
  for (const auto& query : supported) require(CssThemeSheet::supportsMatches(query), "supported query: " + query);
  const QStringList rejected = {
      "(display: contents)", "(display: invalid)", "(color: nonsense)", "(padding: nonsense)",
      "(backdrop-filter: blur(nonsense))", "(filter: blur(1px) unsupported(2))", "(filter: blur(-1px))",
      "(counter-reset: reversed(heading))", "(background-image: linear-gradient(nonsense))", "selector(:unknown)", "not garbage", "not (display: grid", "(display: grid) trailing",
      "(display: grid), (color: red)", "(display: grid) and (color: red) or (opacity: .5)",
      "not (display: grid) and (color: red)", "(display: grid)and(color: red)", "(color:red;display:grid)", "/* missing"};
  for (const auto& query : rejected) require(!CssThemeSheet::supportsMatches(query), "invalid/unsupported query: " + query);

  const auto sheet = CssThemeParser::parse(QStringLiteral(
      ":root{--accent:#111} p{color:var(--accent)}"
      "@supports (unknown:x){:root{--accent:#f00} @keyframes rejected{from{opacity:0}to{opacity:1}}}"
      "@supports (display:grid){:root{--accent:#123456}"
      " @media (min-width:800px){p{padding:12px} @supports not (unknown:x){p{margin:3px}}}"
      " @supports (unknown:x){p{color:#0f0}}}"
      "@supports (display:invalid){p{color:#00f}}"), {});
  CssElement html; html.tag = "html";
  CssElement p; p.tag = "p"; p.parent = &html;
  CssEnvironment wide; wide.viewportWidth = 1024;
  const auto style = CssComputedStyleEngine(sheet, wide).styleFor(p);
  require(style.resolvedValue("color") == "#123456", QStringLiteral("conditional root variables follow cascade without leaking"));
  require(style.length("padding-left").px == 12 && style.length("margin-left").px == 3,
          QStringLiteral("nested supports/media preserve source order and conditions"));
  require(sheet.keyframes().empty(), QStringLiteral("unsupported branch must not register animations"));
  CssEnvironment narrow = wide; narrow.viewportWidth = 600;
  require(CssComputedStyleEngine(sheet, narrow).styleFor(p).length("padding-left").px != 12,
          QStringLiteral("supports branch retains responsive media condition"));
  require(!sheet.sameActiveRules(wide, narrow), QStringLiteral("environment changes detect nested conditional rules"));
  const auto evaluated = sheet.evaluated(wide);
  for (const auto& rule : evaluated.rules()) require(rule.supportQueries.isEmpty(), QStringLiteral("evaluated sheets consume conditions"));
  return 0;
}

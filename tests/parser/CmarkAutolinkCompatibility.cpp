#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include "CmarkAutolinkSnapshot.h"
#include "ParserTestUtils.h"

void testCmarkAutolinkCompatibility(const QString& footnoteOraclePath) {
  const auto directory = QFileInfo(footnoteOraclePath).dir();
  QFile inputFile(directory.filePath(QStringLiteral("cmark-autolink-inputs.json")));
  QFile oracleFile(directory.filePath(QStringLiteral("cmark-autolink-oracle.json")));
  require(inputFile.open(QIODevice::ReadOnly) && oracleFile.open(QIODevice::ReadOnly),
          QStringLiteral("Cannot open autolink original-library fixtures"));
  const auto bytes = inputFile.readAll();
  const auto inputs = QJsonDocument::fromJson(bytes).array();
  const auto oracle = QJsonDocument::fromJson(oracleFile.readAll()).object();
  require(QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) ==
              oracle["provenance"].toObject()["inputs_sha256"].toString(),
          QStringLiteral("Autolink inputs no longer match the original-library oracle"));
  const auto expected = oracle["records"].toArray();
  const auto actual = autolink_test::run(inputs);
  require(inputs.size() == 217 && actual.size() == 1736 && expected.size() == actual.size(),
          QStringLiteral("Incomplete autolink semantic matrix"));
  for (qsizetype index = 0; index < actual.size(); ++index) {
    const auto row = actual[index].toObject();
    require(row == expected[index].toObject(),
            QStringLiteral("Autolink AST/HTML/coordinates differ from original: %1, %2, chunks=%3")
                .arg(row["name"].toString())
                .arg(row["mode"].toString())
                .arg(row["byte_chunks"].toBool()));
  }

  // The same extension object may appear twice with another postprocessor
  // between its attachments. Only its first invocation can be fused.
  auto* parser = footnote_test::parser(true);
  auto* injector = cmark_syntax_extension_new("autolink-sandwich-regression");
  cmark_syntax_extension_set_postprocess_func(injector, autolink_test::inject);
  cmark_parser_attach_syntax_extension(parser, injector);
  cmark_parser_attach_syntax_extension(parser, cmark_find_syntax_extension("autolink"));
  const auto sandwich = footnote_test::snapshot(parser, QByteArray("plain\n"), 1, true);
  require(sandwich["html"].toString().contains(QStringLiteral("href=\"mailto:injected@example.com\"")),
          QStringLiteral("Later autolink attachment must process text created by an intervening extension"));
  cmark_parser_free(parser);
  cmark_syntax_extension_free(cmark_get_default_mem_allocator(), injector);
}

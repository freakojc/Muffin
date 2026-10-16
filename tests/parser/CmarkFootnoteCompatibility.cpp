#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include "CmarkFootnoteSnapshot.h"
#include "../TestUtils.h"

void testCmarkFootnoteCompatibility(const QString& oraclePath) {
  QFile oracleFile(oraclePath);
  require(oracleFile.open(QIODevice::ReadOnly), QStringLiteral("Cannot read cmark footnote oracle"));
  const auto oracle = QJsonDocument::fromJson(oracleFile.readAll()).object();
  QFile inputFile(QFileInfo(oraclePath).dir().filePath(QStringLiteral("cmark-footnote-inputs.json")));
  require(inputFile.open(QIODevice::ReadOnly), QStringLiteral("Cannot read cmark footnote inputs"));
  const auto bytes = inputFile.readAll();
  require(QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) ==
              oracle["provenance"].toObject()["inputs_sha256"].toString(),
          QStringLiteral("Footnote inputs no longer match the original-library oracle"));
  const auto inputs = QJsonDocument::fromJson(bytes).array();
  const auto records = oracle["records"].toArray();
  require(inputs.size() == 159 && records.size() == inputs.size() * 4,
          QStringLiteral("Incomplete cmark footnote compatibility matrix"));
  qsizetype index = 0;
  for (const bool enabled : {true, false}) {
    auto* parser = footnote_test::parser(enabled);
    for (int repeat = 0; repeat < 2; ++repeat)
      for (const auto& entry : inputs) {
        const auto input = entry.toObject();
        const auto source = input["source"].toString().toUtf8();
        const auto record = records[index++].toObject();
        require(record["name"] == input["name"] && record["enabled"].toBool() == enabled &&
                    record["byte_chunks"].toBool() == bool(repeat),
                QStringLiteral("Misordered cmark footnote oracle"));
        if (qEnvironmentVariableIsSet("MUFFIN_CMARK_ORACLE_TRACE"))
          fprintf(stderr, "Footnote oracle %s enabled=%d chunks=%d\n", qPrintable(input["name"].toString()), enabled,
                  repeat);
        const auto actual = footnote_test::snapshot(parser, source, repeat ? 1 : qMax(1, int(source.size())));
        require(
            actual == record["result"].toObject(),
            QStringLiteral("Footnote AST/HTML/source coordinates differ from original cmark: %1, enabled=%2, chunks=%3")
                .arg(input["name"].toString())
                .arg(enabled)
                .arg(bool(repeat)));
      }
    cmark_parser_free(parser);
  }
}

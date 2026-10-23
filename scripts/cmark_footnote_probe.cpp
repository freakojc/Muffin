// Run before changing cmark to capture an independent, ordinary-archive oracle.
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <cstdio>

#include "../tests/parser/CmarkFootnoteSnapshot.h"

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc != 2)
    return 2;
  QFile input(QString::fromLocal8Bit(argv[1]));
  if (!input.open(QIODevice::ReadOnly))
    return 3;
  const auto cases = QJsonDocument::fromJson(input.readAll()).array();
  QJsonArray records;
  // Reuse each parser across footnote/non-footnote documents to cover reset.
  for (const bool enabled : {true, false}) {
    auto* parser = footnote_test::parser(enabled);
    for (int repeat = 0; repeat < 2; ++repeat)
      for (const auto& entry : cases) {
        auto row = entry.toObject();
        const auto source = row["source"].toString().toUtf8();
        const auto result = footnote_test::snapshot(parser, source, repeat ? 1 : qMax(1, int(source.size())));
        records.append(QJsonObject{
            {"name", row["name"]}, {"enabled", enabled}, {"byte_chunks", bool(repeat)}, {"result", result}});
      }
    cmark_parser_free(parser);
  }
  std::printf("%s\n", QJsonDocument(records).toJson(QJsonDocument::Compact).constData());
}

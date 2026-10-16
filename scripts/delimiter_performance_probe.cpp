// Manual headless phase benchmark. Times parsing with coarse phase metrics,
// independently of GUI delivery. Arguments: MiB, repetitions per paragraph,
// iterations. MUFFIN_DELIMITER_PROFILE adds aggregate token diagnostics.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <cstdio>

#include "parser/CmarkGfmParser.h"

namespace {
QJsonArray delimiterProfiles;
QJsonArray adapterProfiles;
void messages(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  if (qstrcmp(context.category, "muffin.adapter") == 0) {
    adapterProfiles.append(QJsonDocument::fromJson(message.toUtf8()).object());
    return;
  }
  if (qstrcmp(context.category, "muffin.delimiter") == 0) {
    delimiterProfiles.append(QJsonDocument::fromJson(message.toUtf8()).object());
    return;
  }
  if (type >= QtWarningMsg) std::fprintf(stderr, "%s\n", qPrintable(message));
  if (type == QtFatalMsg) std::abort();
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc != 4) return 2;
  const int mib = QString::fromLocal8Bit(argv[1]).toInt();
  const int repetitions = QString::fromLocal8Bit(argv[2]).toInt();
  const int iterations = QString::fromLocal8Bit(argv[3]).toInt();
  if (mib < 1 || mib > 100 || repetitions < 1 || repetitions > 10000 || iterations < 1 || iterations > 20) return 2;
  QLoggingCategory::setFilterRules(QStringLiteral("muffin.perf*.debug=false"));
  qInstallMessageHandler(messages);
  const auto paragraph =
      QStringLiteral("Plain text with **bold**, *italic*, ~~strike~~, `code`, and [Muffin](https://example.com). ")
          .repeated(repetitions) +
      "\n\n";
  QString text;
  while (text.size() < qsizetype(mib) * 1024 * 1024) text += paragraph;
  QJsonArray rows;
  for (int i = 0; i < iterations; ++i) {
    delimiterProfiles = {};
    adapterProfiles = {};
    QElapsedTimer timer;
    timer.start();
    auto parsed = muffin::CmarkGfmParser().parseDocumentProfiled(text, {});
    const double wallMs = timer.nsecsElapsed() / 1e6;
    if (!parsed.root) return 3;
    QJsonArray phases;
    for (const auto& phase : parsed.performanceMetrics.phases)
      phases.append(QJsonObject{{"name", phase.name},
                                {"duration_ms", phase.elapsedNs / 1e6},
                                {"working_set_bytes", phase.workingSetBytesAfter}});
    const auto blocks = qsizetype(parsed.root->children().size());
    timer.restart();
    parsed.root.reset();
    rows.append(QJsonObject{{"wall_ms", wallMs},
                            {"release_ms", timer.nsecsElapsed() / 1e6},
                            {"blocks", blocks},
                            {"phases", phases},
                            {"adapter_profile", adapterProfiles},
                            {"delimiter_profile", delimiterProfiles}});
  }
  const QJsonObject result{
      {"MiB", mib}, {"text_chars", text.size()}, {"repetitions_per_paragraph", repetitions}, {"iterations", rows}};
  std::printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
  return 0;
}

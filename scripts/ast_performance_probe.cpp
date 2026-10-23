// Manual Core-only document preparation benchmark. Use the same EXE with
// baseline/candidate DLLs; GUI open-to-paint measurement is a separate probe.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <cstdio>

#include "diagnostics/ProcessMemory.h"
#include "document/DocumentSession.h"

namespace {
QJsonArray adapterProfiles;
void messages(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  if (qstrcmp(context.category, "muffin.adapter") == 0) {
    adapterProfiles.append(QJsonDocument::fromJson(message.toUtf8()).object());
    return;
  }
  if (type >= QtWarningMsg)
    std::fprintf(stderr, "%s\n", qPrintable(message));
  if (type == QtFatalMsg)
    std::abort();
}
void countInlines(const QVector<muffin::InlineNode>& nodes, qint64& total, qint64& shared) {
  for (const auto& node : nodes) {
    ++total;
    shared += node.usesSharedText();
    countInlines(node.children(), total, shared);
  }
}
void countBlocks(const muffin::MarkdownNode& node, qint64& blocks, qint64& inlines, qint64& shared) {
  ++blocks;
  countInlines(node.inlines(), inlines, shared);
  for (const auto& child : node.children())
    countBlocks(*child, blocks, inlines, shared);
}
void fingerprintInlines(QCryptographicHash& hash, const QVector<muffin::InlineNode>& nodes) {
  QByteArray bytes;
  QDataStream out(&bytes, QIODevice::WriteOnly);
  out.setVersion(QDataStream::Qt_6_0);
  out << qint64(nodes.size());
  for (const auto& node : nodes) {
    out << qint32(node.type()) << node.text() << node.marker() << node.href() << node.title() << node.alt()
        << node.isAutolink() << node.usesSharedText();
    const auto ranges = node.sourceRanges();
    for (const auto& range : {ranges.source, ranges.content, ranges.openMarker, ranges.closeMarker})
      out << qint64(range.start) << qint64(range.end);
    // Bound temporary storage to one inline sibling list.
    hash.addData(bytes);
    bytes.clear();
    out.device()->seek(0);
    fingerprintInlines(hash, node.children());
  }
  hash.addData(bytes);
}
void fingerprintBlock(QCryptographicHash& hash, const muffin::MarkdownNode& node) {
  QByteArray bytes;
  QDataStream out(&bytes, QIODevice::WriteOnly);
  out.setVersion(QDataStream::Qt_6_0);
  const auto range = node.sourceRange();
  out << qint32(node.type()) << node.literal() << qint64(range.byteStart) << qint64(range.byteEnd)
      << qint32(range.lineStart) << qint32(range.lineEnd) << qint64(node.children().size());
  hash.addData(bytes);
  fingerprintInlines(hash, node.inlines());
  for (const auto& child : node.children())
    fingerprintBlock(hash, *child);
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc != 3)
    return 2;
  const int mib = QString::fromLocal8Bit(argv[1]).toInt();
  const QString workload = QString::fromLocal8Bit(argv[2]);
  if (mib < 1 || mib > 100 || (workload != "ascii" && workload != "unicode" && workload != "escaped"))
    return 2;
  QLoggingCategory::setFilterRules(QStringLiteral("muffin.perf*.debug=false"));
  qInstallMessageHandler(messages);
  QString sentence =
      QStringLiteral("Plain text with **bold**, *italic*, ~~strike~~, `code`, and [Muffin](https://example.com). ");
  if (workload == "unicode")
    sentence.prepend(QString::fromUtf8("中文 \xf0\x9f\x98\x80 "));
  if (workload == "escaped")
    sentence += QStringLiteral("&amp; &#126; \\*literal\\* `  padded  ` <kbd>Ctrl</kbd> ");
  const QString paragraph = sentence.repeated(12) + "\n\n";
  QString text;
  while (text.size() < qsizetype(mib) * 1024 * 1024)
    text += paragraph;
  auto session = std::make_unique<muffin::DocumentSession>();
  session->setFullParsePerformanceMetricsEnabled(true);
  adapterProfiles = {};
  QElapsedTimer timer;
  timer.start();
  session->setMarkdownText(text, false);
  const double wallMs = timer.nsecsElapsed() / 1e6;
  const auto metrics = session->lastFullParsePerformanceMetrics();
  QJsonArray phases;
  for (const auto& phase : metrics.phases)
    phases.append(QJsonObject{{"name", phase.name},
                              {"duration_ms", phase.elapsedNs / 1e6},
                              {"working_set_bytes", phase.workingSetBytesAfter}});
  qint64 blocks = 0, inlines = 0, shared = 0;
  countBlocks(session->document().root(), blocks, inlines, shared);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  fingerprintBlock(hash, session->document().root());
  const auto workingSet = muffin::diag::workingSetBytes();
  timer.restart();
  session.reset();
  const QJsonObject result{{"MiB", mib},
                           {"workload", workload},
                           {"text_chars", text.size()},
                           {"wall_ms", wallMs},
                           {"parse_ms", metrics.totalElapsedNs / 1e6},
                           {"phases", phases},
                           {"blocks", blocks},
                           {"inlines", inlines},
                           {"shared_inlines", shared},
                           {"ast_fingerprint", QString::fromLatin1(hash.result().toHex())},
                           {"working_set_bytes", workingSet},
                           {"release_ms", timer.nsecsElapsed() / 1e6},
                           {"adapter_profile", adapterProfiles}};
  std::printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
  return 0;
}

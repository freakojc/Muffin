// cmark-only stages. Workload generation, verification and release are untimed.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "../src/diagnostics/ProcessMemory.h"
#include "cmark_stage_profile.h"
extern "C" {
#include "cmark-gfm-core-extensions.h"
#include "cmark-gfm-extension_api.h"
}

extern "C" uint64_t muffin_cmark_clock_ns(void) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
namespace {
QJsonObject memory() {
  QJsonObject result{{"working_set_bytes", muffin::diag::workingSetBytes()}};
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters))) {
    result["private_commit_bytes"] = qint64(counters.PrivateUsage);
    result["peak_working_set_bytes"] = qint64(counters.PeakWorkingSetSize);
  }
#endif
  return result;
}
void check(bool condition, const char* message) {
  if (!condition)
    qFatal("cmark probe: %s", message);
}
QJsonObject fingerprint(cmark_node* root) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  auto* iterator = cmark_iter_new(root);
  qint64 nodes = 0;
  cmark_event_type event;
  while ((event = cmark_iter_next(iterator)) != CMARK_EVENT_DONE) {
    auto* node = cmark_iter_get_node(iterator);
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << qint32(event) << QByteArray(cmark_node_get_type_string(node)) << QByteArray(cmark_node_get_literal(node))
        << QByteArray(cmark_node_get_url(node)) << QByteArray(cmark_node_get_title(node))
        << cmark_node_get_start_line(node) << cmark_node_get_start_column(node) << cmark_node_get_end_line(node)
        << cmark_node_get_end_column(node);
    hash.addData(bytes);
    if (event == CMARK_EVENT_ENTER)
      ++nodes;
  }
  cmark_iter_free(iterator);
  return {{"nodes", nodes}, {"sha256", QString::fromLatin1(hash.result().toHex())}};
}
QString workload(int mib, const QString& kind) {
  const auto paragraph =
      QString("Plain text with **bold**, *italic*, ~~strike~~, `code`, and [Muffin](https://example.com). ")
          .repeated(12) +
      "\n\n";
  QString text;
  const auto syntax = QString::fromUtf8(
      "# Theme acceptance 标题\n\nPlain text 中英文 with **bold**, *italic*, ~~strike~~, `code`, "
      "[Muffin](https://example.com), <kbd>Ctrl</kbd> and $E=mc^2$.\n\n"
      "## Section heading\n\n> A quotation with **strong text**.\n\n1. First item\n2. Second item\n   1. Nested "
      "item\n\n- [x] Checked task\n- [ ] Pending task\n\n");
  if (kind.startsWith("gui"))
    text = syntax;
  const auto block = kind == "extensions"
                         ? syntax + "| a | b |\n|---|---|\n| **bold** | $x$ |\n\nhttps://example.com www.example.com "
                                    "a@example.com\n\n[^a]: footnote\n\nReference [^a].\n\n$$\nx+y\n$$\n\n"
                     : kind == "plain" ? QString("Plain text without inline syntax. ").repeated(36) + "\n\n"
                                       : paragraph;
  while (text.size() < qsizetype(mib) * 1024 * 1024) {
    if (kind.startsWith("gui") && text.size() % 65536 < 1200)
      text += "# Heading\n\n";
    text += block;
  }
  if (kind == "gui-footnote")
    text += "Reference [^a].\n\n[^a]: A footnote with **bold**.\n";
  if (kind == "gui-undefined")
    text += "Reference [^missing].\n";
  if (kind == "gui-email")
    text += "Emails a@example.com, mailto:b@example.org and c&#64;example.net.\n";
  if (kind == "gui-noise")
    text += "Not emails @@@ invalid@host and `code@example.com`.\n";
  return text;
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc != 5)
    return 2;
  const int mib = QString::fromLocal8Bit(argv[1]).toInt();
  const auto kind = QString::fromLocal8Bit(argv[2]), mode = QString::fromLocal8Bit(argv[3]);
  const bool enabled = QString::fromLocal8Bit(argv[4]) == "stages";
#ifdef MUFFIN_CMARK_ARCHIVE_CONTROL
  if (enabled)
    return 2;
#endif
  if (mib < 1 || mib > 30 ||
      !QStringList{"gui", "gui-footnote", "gui-undefined", "gui-email", "gui-noise", "plain", "extensions"}.contains(
          kind) ||
      !QStringList{"full", "no-autolink", "none"}.contains(mode) ||
      !QStringList{"stages", "off"}.contains(QString::fromLocal8Bit(argv[4])))
    return 2;
  const auto text = workload(mib, kind);
  const auto bytes = text.toUtf8();
  cmark_gfm_core_extensions_ensure_registered();
  const auto preparedMemory = memory();
  QElapsedTimer timer;
  timer.start();
  auto* parser = cmark_parser_new(CMARK_OPT_DEFAULT | CMARK_OPT_FOOTNOTES);
  QJsonArray extensions;
  if (mode != "none")
    for (const auto* name : {"table", "autolink", "tasklist", "math"}) {
      if (mode == "no-autolink" && QByteArray(name) == "autolink")
        continue;
      auto* extension = cmark_find_syntax_extension(name);
      check(extension && cmark_parser_attach_syntax_extension(parser, extension), "cannot attach production extension");
      extensions.append(name);
    }
  const double setupMs = timer.nsecsElapsed() / 1e6;
  muffin_cmark_profile profile{};
#ifndef MUFFIN_CMARK_ARCHIVE_CONTROL
  if (enabled)
    muffin_cmark_profile_begin(parser, &profile);
#endif
  timer.restart();
  cmark_parser_feed(parser, bytes.constData(), size_t(bytes.size()));
  const double feedMs = timer.nsecsElapsed() / 1e6;
  timer.restart();
  auto* root = cmark_parser_finish(parser);
  const double finishMs = timer.nsecsElapsed() / 1e6;
  const auto parsedMemory = memory();
#ifndef MUFFIN_CMARK_ARCHIVE_CONTROL
  if (enabled)
    muffin_cmark_profile_end();
#endif
  check(root != nullptr, "missing tree");
  const auto tree = fingerprint(root);
  auto* html = cmark_render_html(root, CMARK_OPT_DEFAULT, cmark_parser_get_syntax_extensions(parser));
  check(html != nullptr, "missing HTML reference");
  const auto htmlHash = QCryptographicHash::hash(QByteArrayView(html), QCryptographicHash::Sha256).toHex();
  std::free(html);
  timer.restart();
  cmark_node_free(root);
  cmark_parser_free(parser);
  const double releaseMs = timer.nsecsElapsed() / 1e6;
  const auto releasedMemory = memory();
  const QJsonObject result{{"MiB", mib},
                           {"workload", kind},
                           {"extensions", extensions},
                           {"mode", mode},
                           {"profiling", enabled},
                           {"text_chars", text.size()},
                           {"utf8_bytes", bytes.size()},
                           {"tree", tree},
                           {"html_sha256", QString::fromLatin1(htmlHash)},
#ifdef MUFFIN_CMARK_ARCHIVE_CONTROL
                           {"implementation", "ordinary_archive"},
#else
                           {"implementation", "diagnostic_block_object"},
#endif
                           {"setup_ms", setupMs},
                           {"feed_ms", feedMs},
                           {"finish_ms", finishMs},
                           {"total_ms", setupMs + feedMs + finishMs},
                           {"inline_ms", profile.inline_ns / 1e6},
                           {"inline_walk_ms", profile.inline_walk_ns / 1e6},
                           {"footnotes_ms", profile.footnotes_ns / 1e6},
                           {"inline_calls", qint64(profile.inline_calls)},
                           {"consolidate_ms", profile.consolidate_ns / 1e6},
                           {"autolink_postprocess_ms", profile.postprocess_ns / 1e6},
                           {"postprocess_calls", qint64(profile.postprocess_calls)},
                           {"autolink_consolidate_ms", profile.autolink_consolidate_ns / 1e6},
                           {"autolink_text_ms", profile.autolink_text_ns / 1e6},
                           {"autolink_text_calls", qint64(profile.autolink_text_calls)},
                           {"autolink_text_bytes", qint64(profile.autolink_text_bytes)},
                           {"autolink_candidates", qint64(profile.autolink_candidates)},
                           {"autolink_visits", qint64(profile.autolink_visits)},
                           {"release_ms", releaseMs},
                           {"prepared_memory", preparedMemory},
                           {"parsed_memory", parsedMemory},
                           {"released_memory", releasedMemory}};
  std::printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}

#pragma once

// Manual ownership/retention audit, invoked by MuffinDocumentTabsTest --memory-audit.
// Object liveness and retained payloads are evidence; OS commit is contextual only.
#include "MemoryAccounting.h"
#include "app/QuickOpenDialog.h"
#include "app/SearchPanelWidget.h"
#include "app/PreferencesDialog.h"
#include "app/PrefsExportPage.h"
#include "editor/ResourceUrl.h"
#include "export/HtmlExporter.h"
#include "export/NativeExporter.h"
#include "mermaid/editor/MermaidRenderCache.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QThreadPool>
#include <QtTest/QTest>

namespace muffin::diag {

inline void drainMemoryAuditEvents() {
  require(QThreadPool::globalInstance()->waitForDone(30000), "audit workers finish within 30 seconds");
  for (int i = 0; i < 4; ++i) {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    QTest::qWait(1);
  }
}

template <class Fixture> void runMemoryLifecycleAudit() {
  Fixture f;
  QJsonArray samples;
  QJsonObject checks;
  const auto sample = [&](const QString& phase, int cycle) {
    drainMemoryAuditEvents();
    QVector<DocumentTab*> tabs;
    for (int i = 0; i < f.tabs->count(); ++i)
      tabs.append(static_cast<DocumentTab*>(f.tabs->widget(i)));
    auto row = MemoryAccounting::process();
    row["phase"] = phase;
    row["cycle"] = cycle;
    row["window_qobjects"] = f.window.template findChildren<QObject*>().size();
    row["all_widgets"] = QApplication::allWidgets().size();
    row["resources"] = MemoryAccounting::resources(f.window, tabs);
    samples.append(row);
    std::fprintf(stderr, "AUDIT %s %d: objects=%d private=%lld image_bytes=%lld\n", qPrintable(phase), cycle,
                 row["window_qobjects"].toInt(), static_cast<long long>(row["private_commit_bytes"].toDouble()),
                 static_cast<long long>(row["resources"].toObject()["image_cache_bytes"].toDouble()));
    std::fflush(stderr);
  };
  const auto plain = f.file("plain.md", QString("# Note\n\nA **bold** note with audit-term.\n\n").repeated(200));
  const auto rich = f.file("rich.md", "# Rich\n\n```cpp\nint main() { return 0; }\n```\n\n"
                                      "$$\nx^2 + y^2 = z^2\n$$\n\n<div><b>HTML</b></div>\n\n"
                                      "| A | B |\n|---|---|\n| 1 | 2 |\n");
  const auto openClose = [&](const QString& path) {
    auto* document = f.open(path);
    QList<QPointer<QObject>> owned{document,
                                   &document->session,
                                   &document->editorController,
                                   &document->editorController.undoStack(),
                                   document->renderView,
                                   document->editor};
    f.close(document);
    drainMemoryAuditEvents();
    for (const auto& object : owned)
      require(object.isNull(), "closing a tab destroys document, controller, views and undo stack");
  };
  for (int i = 0; i < 10; ++i) {
    openClose(plain);
    openClose(rich);
  }
  sample("warm", 0);
  for (int i = 1; i <= 60; ++i) {
    openClose(plain);
    if (i % 10 == 0)
      sample("plain-open-close", i);
  }
  for (int i = 1; i <= 30; ++i) {
    openClose(rich);
    if (i % 10 == 0)
      sample("rich-open-close", i);
  }
  checks["closed_tab_liveness_checks"] = 110 * 6;

  for (int i = 1; i <= 30; ++i) {
    {
      SearchPanelWidget panel;
      panel.setRoot(f.directory.path());
      panel.findChild<QLineEdit*>("WorkspaceSearchQuery")->setText("audit-term");
      panel.startSearch({});
      panel.startSearch({}); // cancel/supersede the first worker
      drainMemoryAuditEvents();
      require(panel.result().complete && panel.result().matches > 0, "audit search returns real matches");
      require(panel.findChildren<QFutureWatcherBase*>().isEmpty(), "finished search watchers are deleted");
    }
    {
      QuickOpenDialog dialog;
      dialog.setWorkspace(f.directory.path(), WorkspaceFileIndex::textExtensions());
      // Destruction also covers a picker closed while its scan is in flight.
    }
    drainMemoryAuditEvents();
    if (i % 10 == 0)
      sample("search-and-picker", i);
  }
  for (int i = 1; i <= 10; ++i) {
    {
      PreferencesDialog dialog;
      auto* page = dialog.findChild<PrefsExportPage*>();
      auto* formats = page->findChild<QListWidget*>("ExportFormats");
      for (int row = 0; row < formats->count(); ++row)
        formats->setCurrentRow(row);
    }
    if (i % 5 == 0)
      sample("preferences-format-switches", i);
  }

  {
    DocumentSession session;
    session.setMarkdownText("# Export\n\nRepeated **content**.\n\n```cpp\nint x = 1;\n```\n", false);
    const auto theme = f.current()->renderView->theme();
    for (int i = 1; i <= 20; ++i) {
      const auto html = renderDocumentHtml(session.markdownText().toString(), "Audit", true);
      require(html.contains("Repeated"), "audit HTML export completes");
      require(exportNativePdf(f.directory.filePath("audit.pdf"), session.document(), theme, {}, pdfExportOptions()),
              "audit PDF export completes");
      require(exportNativeImage(f.directory.filePath("audit.png"), session.document(), theme, {}, imageExportOptions()),
              "audit PNG export completes");
      if (i % 5 == 0)
        sample("html-pdf-png-export", i);
    }
  }

  // Measure reclamation of deleted insertions independently of undo history.
  {
    PieceTable text(QStringLiteral("x"));
    const QString insertion(512 * 1024, QLatin1Char('x'));
    for (int i = 0; i < 32; ++i) {
      text.replace(text.size(), text.size(), insertion);
      text.replace(1, text.size(), {});
    }
    MemoryAccounting payload;
    payload.text(text);
    checks["piece_table_logical_code_units_after_delete"] = qint64(text.size());
    checks["piece_table_backing_buffer_capacity_bytes_after_delete"] = payload.sharedSource;
    checks["piece_table_insert_delete_cycles"] = 32;
    checks["piece_table_inserted_then_deleted_code_units"] = qint64(insertion.size() * 32);
    require(text.size() == 1, "piece-table fixture leaves one visible character");
  }

  // A QString held by the probe lets isDetached() distinguish allocator reserve
  // from another real owner retaining the old buffer after document replacement.
  {
    QString source = "```text\n" + QString(8 * 1024 * 1024, QLatin1Char('x')) + "\n```\n";
    require(source.isDetached(), "audit source initially has a single owner");
    auto session = std::make_unique<DocumentSession>();
    session->openDocumentAsync(source);
    drainMemoryAuditEvents();
    require(!session->isAsyncParseInProgress(), "audit async open finishes");
    session->newDocument();
    drainMemoryAuditEvents();
    checks["empty_session_retains_previous_async_source"] = !source.isDetached();
    checks["retained_async_source_bytes"] = source.isDetached() ? 0 : qint64(source.size() * sizeof(QChar));
    session.reset();
    drainMemoryAuditEvents();
    checks["async_source_released_after_session_destruction"] = source.isDetached();
    require(source.isDetached(), "destroying the session releases its source owners");
  }

  QStringList resourceKeys;
  int expiredScenes = 0;
  for (int i = 1; i <= 100; ++i) {
    std::weak_ptr<const mermaid::MermaidScene> scene;
    {
      mermaid::editor::MermaidRenderCache cache;
      const QString source = "pie\n  \"A\" : 1\n";
      const auto key = cache.makeKey(source);
      auto entry = cache.getSync(key, source);
      require(bool(entry.scene), "audit diagram renders");
      scene = entry.scene;
      resourceKeys.append(cache.resourceKey(key));
    }
    if (scene.expired())
      ++expiredScenes;
  }
  int retainedResourceVersions = 0;
  for (const auto& key : resourceKeys)
    retainedResourceVersions += LayoutResources::instance().read(key).paint != 0;
  checks["mermaid_scenes_released_after_cache_destruction"] = expiredScenes;
  checks["orphaned_mermaid_resource_versions"] = retainedResourceVersions;
  sample("mermaid-cache-lifetimes", 100);

  int retainedImagesAfterClose = 0;
  for (int i = 1; i <= 24; ++i) {
    const auto name = QString("asset-%1.png").arg(i);
    const auto imagePath = f.directory.filePath(name);
    {
      QImage image(1024, 1024, QImage::Format_ARGB32);
      image.fill(QColor::fromRgb(i * 7919));
      require(image.save(imagePath), "audit image fixture saves");
    }
    const auto path = f.file(QString("image-%1.md").arg(i), "# Image\n\n![asset](" + name + ")\n");
    auto* document = f.open(path);
    const auto url = resolvedUrlForDocumentResource(name, path).toString(QUrl::FullyEncoded);
    QElapsedTimer timer;
    timer.start();
    while (ImageLoader::instance().cached(url).isNull() && timer.elapsed() < 5000)
      QTest::qWait(5);
    require(!ImageLoader::instance().cached(url).isNull(), "the real editor loads the image");
    QPointer<DocumentTab> closed(document);
    f.close(document);
    drainMemoryAuditEvents();
    require(closed.isNull(), "image tab is destroyed");
    retainedImagesAfterClose += !ImageLoader::instance().cached(url).isNull();
    if (i % 6 == 0)
      sample("distinct-images-open-close", i);
  }
  checks["closed_image_tabs"] = 24;
  checks["images_retained_after_their_tabs_close"] = retainedImagesAfterClose;
  checks["bytes_per_distinct_image"] = 1024 * 1024 * 4;
  QJsonObject report{
      {"method",
       "QObject/QPointer liveness, QString ownership, weak_ptr scene lifetime, cache payload census; OS commit is not a leak detector"},
      {"checks", checks},
      {"samples", samples}};
  const auto output = qEnvironmentVariable("MUFFIN_MEMORY_AUDIT_OUTPUT");
  require(!output.isEmpty(), "MUFFIN_MEMORY_AUDIT_OUTPUT must name the JSON report");
  QFile file(output);
  require(file.open(QIODevice::WriteOnly), "audit report opens");
  require(file.write(QJsonDocument(report).toJson()) > 0, "audit report writes");
}

} // namespace muffin::diag

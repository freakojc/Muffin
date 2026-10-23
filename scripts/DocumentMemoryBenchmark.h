#pragma once

// Manual, fresh-process GUI benchmark. Run --document-memory with
// MUFFIN_BENCH_FILE and MUFFIN_MEMORY_BENCH_OUTPUT. Census runs after sampling.
#include "MemoryAccounting.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest/QTest>

#include <atomic>
#include <chrono>
#include <thread>

namespace muffin::diag {

template <class Fixture> void runDocumentMemoryBenchmark() {
  Fixture f;
  QTest::qWait(500);
  const auto baseline = MemoryAccounting::process();
  std::atomic<bool> sampling{true};
  qint64 peakWorkingSet = 0, peakPrivate = 0;
  std::thread sampler([&] {
    while (sampling.load(std::memory_order_relaxed)) {
      const auto sample = MemoryAccounting::process();
      peakWorkingSet = qMax(peakWorkingSet, qint64(sample["working_set_bytes"].toDouble()));
      peakPrivate = qMax(peakPrivate, qint64(sample["private_commit_bytes"].toDouble()));
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  QElapsedTimer timer;
  timer.start();
  const QString path = qEnvironmentVariable("MUFFIN_BENCH_FILE");
  require(f.window.openFile(path), "benchmark file opens");
  auto* tab = f.current();
  while (tab->session.isAsyncParseInProgress() && timer.elapsed() < 120000) {
    QApplication::processEvents();
    QTest::qWait(5);
  }
  require(!tab->session.isAsyncParseInProgress(), "benchmark parse finishes");
  QApplication::processEvents();
  const qint64 openMs = timer.elapsed();
  QTest::qWait(2000);
  QJsonArray stable;
  for (int i = 0; i < 10; ++i) {
    stable.append(MemoryAccounting::process());
    QTest::qWait(100);
  }
  sampling.store(false, std::memory_order_relaxed);
  sampler.join();
  const auto& doc = tab->session.document();
  QJsonObject result{{"file", path},
                     {"baseline", baseline},
                     {"sampled_peak_working_set_bytes", peakWorkingSet},
                     {"sampled_peak_private_commit_bytes", peakPrivate},
                     {"open_ms", openMs},
                     {"stable_samples", stable},
                     {"document", MemoryAccounting().document(doc)},
                     {"layout", MemoryAccounting::layout(*tab->renderView)},
                     {"code_units", doc.pieceText().size()},
                     {"lines", doc.pieceText().lineCount()},
                     {"words", doc.pieceText().wordCount()}};
  QFile input(path);
  require(input.open(QIODevice::ReadOnly), "hash input");
  QCryptographicHash hash(QCryptographicHash::Sha256);
  require(hash.addData(&input), "hash fixture");
  result["sha256"] = QString::fromLatin1(hash.result().toHex());
  QFile output(qEnvironmentVariable("MUFFIN_MEMORY_BENCH_OUTPUT"));
  require(output.open(QIODevice::WriteOnly), "benchmark output");
  output.write(QJsonDocument(result).toJson());
}

} // namespace muffin::diag

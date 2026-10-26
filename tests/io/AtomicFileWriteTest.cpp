#include "../AtomicFileWrite.h"
#include "../TestUtils.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <thread>

namespace {

QByteArray readBytes(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "read atomic write fixture");
  return file.readAll();
}

void replacementAndErrors() {
  QTemporaryDir directory;
  require(directory.isValid(), "atomic writer fixture directory");
  const auto path = directory.filePath(QString::fromUtf8("中文 note.md"));
  QString error;
  require(muffin::test::writeFileAtomically(path, "original", &error), error);
  require(muffin::test::writeFileAtomically(path, "replacement", &error), error);
  require(error.isEmpty() && readBytes(path) == "replacement", "replacement installs the complete byte stream");
  require(muffin::test::writeFileAtomically(path, {}, &error), error);
  require(readBytes(path).isEmpty(), "empty replacement truncates through an atomic commit");
  const auto missing = directory.filePath(QStringLiteral("missing/target.md"));
  require(!muffin::test::writeFileAtomically(missing, "data", &error) && error.startsWith(QStringLiteral("open:")),
          "an invalid destination reports its actual failing operation");
  require(!QFile::exists(missing), "failed writes never manufacture a missing destination");
}

#ifdef Q_OS_WIN
// TestUtils.h provides windows.h. Denying delete sharing reproduces the same
// QSaveFile rename failure as a transient scanner/reader on a Windows runner.
HANDLE blockReplacement(const QString& path) {
  HANDLE handle =
      CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "open a reader without delete sharing");
  return handle;
}

void windowsReplacementContention() {
  QTemporaryDir directory;
  require(directory.isValid(), "Windows sharing fixture directory");
  const auto path = directory.filePath(QStringLiteral("contended.md"));
  QString error;
  require(muffin::test::writeFileAtomically(path, "original", &error), error);

  HANDLE held = blockReplacement(path);
  require(!muffin::test::writeFileAtomically(path, "never installed", &error, 0) &&
              error.startsWith(QStringLiteral("commit:")),
          "the non-shared reader reproduces a denied atomic commit");
  require(readBytes(path) == "original", "a denied commit leaves the previous file intact");
  CloseHandle(held);

  held = blockReplacement(path);
  std::thread release([held] {
    QThread::msleep(100);
    CloseHandle(held);
  });
  // A nested event loop here could run the document's autosave or reload queue
  // in the middle of fixture generation. The writer must wait without doing so.
  bool eventDispatched = false;
  QTimer::singleShot(0, [&] { eventDispatched = true; });
  const bool committed = muffin::test::writeFileAtomically(path, "replacement after release", &error);
  release.join();
  require(committed && error.isEmpty() && readBytes(path) == "replacement after release",
          "a briefly blocked replacement succeeds after the reader releases it");
  require(!eventDispatched, "retrying the external writer does not dispatch application events");
  QCoreApplication::processEvents();
  require(eventDispatched, "the pending application callback runs only after the write completes");

  held = blockReplacement(path);
  QElapsedTimer elapsed;
  elapsed.start();
  const bool blocked = muffin::test::writeFileAtomically(path, "must stay uncommitted", &error, 60);
  const qint64 duration = elapsed.elapsed();
  CloseHandle(held);
  require(!blocked && error.startsWith(QStringLiteral("commit:")) && duration >= 60,
          "persistent contention returns failure after the configured retry deadline");
  require(readBytes(path) == "replacement after release", "retry exhaustion never damages the existing content");
  require(QDir(directory.path()).entryList(QDir::Files) == QStringList{QStringLiteral("contended.md")},
          "retry exhaustion cleans up temporary files");
}
#endif

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  runTest("replacementAndErrors", replacementAndErrors);
#ifdef Q_OS_WIN
  runTest("windowsReplacementContention", windowsReplacementContention);
#endif
  return 0;
}

#include "document/DocumentSession.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <cstdio>

using namespace muffin;

namespace {

void require(bool condition, const QString& message) {
  if (!condition) {
    qCritical().noquote() << message;
    std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    std::exit(1);
  }
}

void atomicWrite(const QString& path, const QByteArray& data) {
  QSaveFile file(path);
  require(file.open(QIODevice::WriteOnly), QStringLiteral("Could not open atomic writer"));
  require(file.write(data) == data.size(), QStringLiteral("Could not write replacement"));
  require(file.commit(), QStringLiteral("Could not commit replacement"));
}

bool waitForCount(int& count, int expected) {
  QElapsedTimer timer;
  timer.start();
  while (count < expected && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(10);
  }
  return count >= expected;
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir dir;
  require(dir.isValid(), QStringLiteral("Temp dir invalid"));
  const QString path = dir.filePath(QStringLiteral("watched.md"));
  atomicWrite(path, QByteArrayLiteral("one"));

  DocumentSession session;
  session.setFilePath(path);
  session.recordFileBaseline();
  int changes = 0;
  QObject::connect(&session, &DocumentSession::externalFileChanged, [&changes] { ++changes; });

  atomicWrite(path, QByteArrayLiteral("replacement-two"));
  require(waitForCount(changes, 1), QStringLiteral("First atomic replacement was not detected"));
  session.recordFileBaseline();

  atomicWrite(path, QByteArrayLiteral("replacement-three-longer"));
  require(waitForCount(changes, 2), QStringLiteral("File watch was not re-armed after replacement"));
  session.recordFileBaseline();

  {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), QStringLiteral("Could not open in-place writer"));
    require(file.write("in-place edit with different size") > 0, QStringLiteral("Could not write in-place edit"));
  }
  require(waitForCount(changes, 3), QStringLiteral("In-place external modification was not detected"));
  session.recordFileBaseline();

  require(QFile::remove(path), QStringLiteral("Could not remove watched file"));
  require(waitForCount(changes, 4), QStringLiteral("File deletion was not detected"));
  atomicWrite(path, QByteArrayLiteral("recreated file"));
  require(waitForCount(changes, 5), QStringLiteral("File recreation was not detected"));
  session.recordFileBaseline();

  const int beforeSelfWrites = changes;
  for (int i = 0; i < 200; ++i) {
    {
      DocumentSession::SelfWriteGuard guard(session);
      atomicWrite(path, QByteArrayLiteral("self-write-") + QByteArray::number(i));
      session.recordFileBaseline();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  require(changes == beforeSelfWrites, QStringLiteral("Own atomic saves must not emit external change notifications"));
  return 0;
}

#include "app/ApplicationInstance.h"
#include "../TestUtils.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <memory>
#include <vector>

namespace {
bool waitForExit(QProcess& process) {
  QElapsedTimer timer;
  timer.start();
  while (process.state() != QProcess::NotRunning && timer.elapsed() < 15000) {
    QCoreApplication::processEvents();
    QThread::msleep(5);
  }
  return process.state() == QProcess::NotRunning && process.exitStatus() == QProcess::NormalExit &&
         process.exitCode() == 0;
}

void fileRequestsCrossProcesses() {
  QTemporaryDir directory;
  require(directory.isValid(), "instance fixture directory");
  const auto identity = directory.filePath(QStringLiteral("instance"));
  const QStringList paths{directory.filePath(QString::fromUtf8("中文 path/hello world.md")),
                          directory.filePath(QStringLiteral("a # & quoted\" name.md"))};
  {
    muffin::ApplicationInstance primary(identity);
    require(primary.listen(), "first process owns the listener");
    require(primary.listen(), "starting a listener twice is harmless");
    muffin::ApplicationInstance contender(identity);
    require(!contender.listen(), "another instance cannot replace a live listener");
    muffin::ApplicationInstance independent(identity + QStringLiteral("other-profile"));
    require(independent.listen(), "different profiles do not share a listener");
    QList<QStringList> requests;
    QObject::connect(&primary, &muffin::ApplicationInstance::filesRequested, &primary,
                     [&](const QStringList& received) { requests.append(received); });
    std::vector<std::unique_ptr<QProcess>> launchers;
    for (int i = 0; i < 3; ++i) {
      auto process = std::make_unique<QProcess>();
      process->start(QCoreApplication::applicationFilePath(), QStringList{QStringLiteral("--send"), identity} + paths);
      require(process->waitForStarted(), "external launcher starts");
      launchers.push_back(std::move(process));
    }
    for (const auto& process : launchers)
      require(waitForExit(*process), "launcher exits after acknowledgement");
    QCoreApplication::processEvents();
    require(requests.size() == 3, "simultaneous launchers each deliver exactly one request");
    for (const auto& request : requests)
      require(request == paths, "Unicode, whitespace and punctuation survive IPC without shell parsing");
    require(!contender.forwardFiles({QStringLiteral("relative.md")}), "relative IPC paths are rejected");
    require(!contender.forwardFiles({}), "empty launches are not forwarded");
  }
  muffin::ApplicationInstance replacement(identity);
  require(replacement.listen(), "closing the primary releases the identity for a later launch");
}

void crashedListenerCanBeReplaced() {
  QTemporaryDir directory;
  const auto identity = directory.filePath(QStringLiteral("crash"));
  const auto ready = directory.filePath(QStringLiteral("ready"));
  QProcess process;
  process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--serve"), identity, ready});
  require(process.waitForStarted(), "crash fixture starts");
  QElapsedTimer timer;
  timer.start();
  while (!QFile::exists(ready) && process.state() != QProcess::NotRunning && timer.elapsed() < 15000) {
    QCoreApplication::processEvents();
    QThread::msleep(5);
  }
  require(QFile::exists(ready), "crash fixture has acquired the listener");
  process.kill();
  require(process.waitForFinished(), "crashed fixture exits");
  muffin::ApplicationInstance replacement(identity);
  require(replacement.listen(), "stale process lock and socket are recovered after a crash");
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const auto arguments = app.arguments();
  if (arguments.value(1) == QStringLiteral("--send")) {
    muffin::ApplicationInstance instance(arguments.value(2));
    return instance.forwardFiles(arguments.mid(3)) ? 0 : 1;
  }
  if (arguments.value(1) == QStringLiteral("--serve")) {
    muffin::ApplicationInstance instance(arguments.value(2));
    if (!instance.listen())
      return 1;
    QFile ready(arguments.value(3));
    if (!ready.open(QIODevice::WriteOnly))
      return 2;
    ready.close();
    QTimer::singleShot(15000, &app, &QCoreApplication::quit);
    return app.exec();
  }
  runTest("fileRequestsCrossProcesses", fileRequestsCrossProcesses);
  runTest("crashedListenerCanBeReplaced", crashedListenerCanBeReplaced);
  return 0;
}

// Manual, native-window measurement of the real MainWindow/DocumentTab paths.
// Input delivery is either synthetic Qt events or guarded Win32 keyboard input.
// Paint completion is QWidget backing-store work, not DWM/display presentation.
#include <QAbstractEventDispatcher>
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QPaintEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

#include "GuiQueueTrace.h"
#include "MemoryAccounting.h"
#include "app/LanguageManager.h"
#include "app/MainWindow.h"
#include "diagnostics/ProcessMemory.h"
#include "diagnostics/ThemePerformance.h"
#include "document/MarkdownNode.h"
#include "editor/EditorView.h"
#include "render/BlockLayout.h"
#include "render/InlineLayout.h"
#include "theme/ThemeManager.h"

using namespace muffin;
namespace {
using Clock = std::chrono::steady_clock;
GuiQueueTrace* queueTrace = nullptr;
qint64 nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
double ms(qint64 ns) { return ns / 1e6; }
void check(bool ok, const char* message) {
  if (!ok) qFatal("GUI probe: %s", message);
}
bool samePosition(const CursorPosition& a, const CursorPosition& b) {
  return a.blockId == b.blockId && a.text.nodeId == b.text.nodeId && a.text.textOffset == b.text.textOffset &&
         a.text.sourceOffset == b.text.sourceOffset && a.text.inMeta == b.text.inMeta && a.afterBlock == b.afterBlock;
}

QJsonObject distribution(QVector<double> values) {
  if (values.isEmpty()) return {{"n", 0}};
  std::sort(values.begin(), values.end());
  const auto percentile = [&](double p) {
    return values[qMax<qsizetype>(0, qsizetype(std::ceil(p * values.size())) - 1)];
  };
  double sum = 0;
  for (const auto value : values) sum += value;
  return {{"n", values.size()},        {"p50_ms", percentile(.50)}, {"p95_ms", percentile(.95)},
          {"p99_ms", percentile(.99)}, {"max_ms", values.back()},   {"mean_ms", sum / values.size()}};
}

class ProbeApplication final : public QApplication {
 public:
  using QApplication::QApplication;
  QHash<QObject*, int> viewports;
  QHash<QObject*, int> editorObjects;
  std::function<void(int, double, bool)> painted;
  std::function<int()> activeTab;
  std::function<void(int, int, double)> hiddenEvent;
  bool traceEvents = false;
  std::function<void(QObject*, QKeyEvent*)> beforeNativeKey;
  std::function<void()> afterNativeKey;
  std::function<void(const QString&, int, int, qint64, qint64, QJsonObject, QJsonObject)> eventCompleted;
  bool notify(QObject* object, QEvent* event) override {
    const auto found = viewports.constFind(object);
    const int tab = found == viewports.cend() ? -1 : found.value();
    const bool paint = tab >= 0 && event->type() == QEvent::Paint;
    const bool fullViewportPaint =
        paint && static_cast<QPaintEvent*>(event)->region().contains(static_cast<QWidget*>(object)->rect());
    const int owner = editorObjects.value(object, -1);
    const bool hidden = owner >= 0 && activeTab && owner != activeTab();
    const int eventType = int(event->type());
    const bool trace = traceEvents && (event->type() == QEvent::Timer || event->type() == QEvent::MetaCall ||
                                       event->type() == QEvent::Paint || event->type() == QEvent::UpdateRequest ||
                                       event->type() == QEvent::UpdateLater);
    QString target;
    int interval = -1;
    if (trace) {
      target = QString::fromLatin1(object->metaObject()->className()) + '/' + object->objectName();
      if (auto* timer = qobject_cast<QTimer*>(object)) interval = timer->interval();
    }
    const qint64 start = paint || hidden || trace ? nowNs() : 0;
    const auto cpuBefore = trace && queueTrace ? queueTrace->threadSample() : QJsonObject{};
    const bool nativeKey = event->type() == QEvent::KeyPress && event->spontaneous() && bool(beforeNativeKey);
    if (nativeKey) beforeNativeKey(object, static_cast<QKeyEvent*>(event));
    const bool result = QApplication::notify(object, event);
    if (nativeKey && afterNativeKey) afterNativeKey();
    if (paint && painted) painted(tab, ms(nowNs() - start), fullViewportPaint);
    if (hidden && hiddenEvent) hiddenEvent(owner, eventType, ms(nowNs() - start));
    if (trace && eventCompleted)
      eventCompleted(target, eventType, interval, start, nowNs(), cpuBefore,
                     queueTrace ? queueTrace->threadSample() : QJsonObject{});
    return result;
  }
};

struct Phase {
  QString name;
  int count, intervalMs;
};
struct Sample {
  qint64 due = 0, posted = 0, dispatch = 0, end = 0, paint = 0, paintStart = 0;
  int tab = -1;
  QString detail;
  bool stateChanged = false;
  bool fullViewportPaint = false;
  QJsonObject cpuBefore, cpuAfter;
  ThemePerfSamples stagesBefore{}, stagesPainted{};
};
const auto operationType = QEvent::Type(QEvent::registerEventType());
struct OperationEvent final : QEvent {
  int index;
  qint64 due, posted;
  OperationEvent(int index, qint64 due) : QEvent(operationType), index(index), due(due), posted(nowNs()) {}
};

QHash<QString, QVector<double>>* activeProbes = nullptr;
std::mutex openProbeMutex;
QJsonArray openProbeScopes;
std::atomic<qint64> openProbeStart{0};
void configurePerformanceLogging(bool opening) {
  const auto openMode = qEnvironmentVariable("MUFFIN_GUI_OPEN_PROFILE");
  const bool coarseOpen = opening && !openMode.isEmpty() && openMode != "legacy";
  QLoggingCategory::setFilterRules(
      QString("muffin.perf.debug=%3\nmuffin.perf.css.debug=%1\nmuffin.perf.detail.debug=%2\nmuffin.perf.queue.debug=%2\nmuffin.perf.open.debug=%4")
          .arg(qEnvironmentVariableIsSet("MUFFIN_CSS_DEEP_PROFILE") ? "true" : "false",
               qEnvironmentVariableIsSet("MUFFIN_GUI_TRACE") ? "true" : "false",
               coarseOpen || qEnvironmentVariable("MUFFIN_GUI_PROFILE") == "off" ? "false" : "true",
               opening && openMode == "stages" ? "true" : "false"));
}
QJsonArray* activeTrace = nullptr;
qint64 traceStart = 0;
int traceOperation = -1, traceDropped = 0;
QHash<quintptr, int> traceViewports;
QJsonArray* themeScopeIntervals = nullptr;
void traceRow(QJsonObject row) {
  if (!activeTrace) return;
  if (activeTrace->size() < 65536)
    activeTrace->append(row);
  else
    ++traceDropped;
}
void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  if (qstrcmp(context.category, "muffin.delimiter") == 0 || qstrcmp(context.category, "muffin.adapter") == 0) {
    std::fprintf(stderr, "%s %s\n", context.category, qPrintable(message));
    return;
  }
  if (qstrcmp(context.category, "muffin.perf.open") == 0) {
    const auto start = openProbeStart.load();
    if (start) {
      const auto parts = message.split(' ', Qt::SkipEmptyParts);
      check(parts.size() == 4, "invalid open scope");
      std::lock_guard lock(openProbeMutex);
      openProbeScopes.append(QJsonObject{{"name", parts[0]}, {"duration_ms", parts[1].toDouble()},
                                        {"end_ms", ms(nowNs() - start)}, {"working_set_bytes", parts[3].toLongLong()},
                                        {"gui_thread", QThread::currentThread() == QCoreApplication::instance()->thread()}});
    }
    return;
  }
  if (qstrcmp(context.category, "muffin.perf.queue") == 0) {
    if (activeTrace) {
      const auto parts = message.split(' ');
      if (parts.size() == 6)
        traceRow({{"kind", "update_request"},
                  {"at_ms", ms(nowNs() - traceStart)},
                  {"tab", traceViewports.value(parts[1].toULongLong(), -1)},
                  {"operation", traceOperation},
                  {"rect", QJsonArray{parts[2].toInt(), parts[3].toInt(), parts[4].toInt(), parts[5].toInt()}}});
    }
    return;
  }
  // Background draft I/O has its own perf scopes. GUI-only containers below
  // are deliberately not shared with workers; their times are not GUI stalls.
  if (QCoreApplication::instance() && QThread::currentThread() != QCoreApplication::instance()->thread() &&
      QStringView(QString::fromLatin1(context.category)).startsWith(u"muffin.perf"))
    return;
  if (activeProbes &&
      (qstrcmp(context.category, "muffin.perf") == 0 || qstrcmp(context.category, "muffin.perf.css") == 0 ||
       qstrcmp(context.category, "muffin.perf.detail") == 0)) {
    const auto parts = message.split(' ', Qt::SkipEmptyParts);
    bool ok = false;
    const auto elapsed = parts.size() > 1 ? parts[1].toDouble(&ok) : 0;
    if (ok) (*activeProbes)[parts[0]].append(elapsed);
    if (ok && themeScopeIntervals) {
      const auto end = ms(nowNs() - traceStart);
      themeScopeIntervals->append(QJsonObject{{"name", parts[0]},
                                              {"start_ms", end - elapsed},
                                              {"end_ms", end},
                                              {"duration_ms", elapsed},
                                              {"operation", traceOperation}});
    }
    if (ok && activeTrace && elapsed >= .1) {
      const auto end = ms(nowNs() - traceStart);
      traceRow({{"kind", "scope"},
                {"name", parts[0]},
                {"start_ms", end - elapsed},
                {"end_ms", end},
                {"duration_ms", elapsed},
                {"operation", traceOperation}});
    }
    return;
  }
  if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
    std::fprintf(stderr, "%s\n", qPrintable(message.left(600)));
  if (type == QtFatalMsg) std::abort();
}

QString workload(qsizetype bytes) {
  QString text;
  if (qEnvironmentVariableIsSet("MUFFIN_GUI_THEME_ACCEPTANCE"))
    text = QString::fromUtf8(
        "# Theme acceptance 标题\n\n"
        "Plain text 中英文 with **bold**, *italic*, ~~strike~~, `code`, "
        "[Muffin](https://example.com), <kbd>Ctrl</kbd> and $E=mc^2$.\n\n"
        "## Section heading\n\n> A quotation with **strong text**.\n\n"
        "1. First item\n2. Second item\n   1. Nested item\n\n- [x] Checked task\n- [ ] Pending task\n\n");
  const auto paragraph =
      QString("Plain text with **bold**, *italic*, ~~strike~~, `code`, and [Muffin](https://example.com). ")
          .repeated(12) +
      "\n\n";
  while (text.size() < bytes) {
    if (text.size() % 65536 < 1200) text += "# Heading\n\n";
    text += paragraph;
  }
  return text;
}

class Runner final : public QObject {
 public:
  Runner(ProbeApplication& app, QString output, double mib, int tabCount, int count, QString source)
      : app_(app),
        output_(std::move(output)),
        mib_(mib),
        tabCount_(tabCount),
        count_(count),
        source_(std::move(source)) {
    chainFile_ = qEnvironmentVariable("MUFFIN_GUI_CHAIN_FILE");
    if (chainFile_.isEmpty()) return;
    check(themeAcceptance_, "a GUI chain requires theme acceptance mode");
    QFile plan(chainFile_);
    check(plan.open(QIODevice::ReadOnly), "cannot read GUI chain plan");
    chainPlan_ = QJsonDocument::fromJson(plan.readAll()).object();
    chainIndex_ = qEnvironmentVariableIntValue("MUFFIN_GUI_CHAIN_INDEX");
    const auto jobs = chainPlan_["jobs"].toArray();
    check(chainPlan_["schema"].toInt() == 1 && chainIndex_ >= 0 && chainIndex_ < jobs.size(),
          "invalid GUI chain index");
    check(QFileInfo(chainPlan_["executable"].toString())
                  .canonicalFilePath()
                  .compare(QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath(),
                           Qt::CaseInsensitive) == 0,
          "GUI chain may only launch this benchmark executable");
    for (const auto jobValue : jobs) {
      const auto job = jobValue.toObject();
      check(QFileInfo(job["output"].toString()).absolutePath() == QFileInfo(chainFile_).absolutePath() &&
                job["mib"].toInt() >= 1 && job["mib"].toInt() <= 30 && job["tabs"].toInt() >= 1 &&
                job["tabs"].toInt() <= 3 && job["cycles"].toInt() >= 4 && job["cycles"].toInt() <= 60 &&
                (job["profile"] == "minimal" || job["profile"] == "off"),
            "GUI chain job escapes its output directory or has invalid dimensions");
    }
    const auto job = jobs[chainIndex_].toObject();
    check(QFileInfo(job["output"].toString()).absoluteFilePath() == output_ && job["mib"].toDouble() == mib_ &&
              job["tabs"].toInt() == tabCount_ && job["cycles"].toInt() == count_ &&
              job["profile"].toString() == qEnvironmentVariable("MUFFIN_GUI_PROFILE"),
          "GUI chain invocation differs from its plan");
#ifdef Q_OS_WIN
    const auto predecessor = qEnvironmentVariable("MUFFIN_GUI_PREDECESSOR_PID").toULong();
    if (predecessor) {
      predecessorHandle_ = OpenProcess(SYNCHRONIZE, FALSE, predecessor);
      check(predecessorHandle_ || GetLastError() == ERROR_INVALID_PARAMETER, "cannot verify predecessor exit");
    }
#endif
  }
  ~Runner() override {
    activeProbes = nullptr;
    activeTrace = nullptr;
    themeScopeIntervals = nullptr;
    app_.painted = {};
    app_.activeTab = {};
    app_.hiddenEvent = {};
    app_.eventCompleted = {};
    app_.beforeNativeKey = {};
    app_.afterNativeKey = {};
    if (queueTrace) queueTrace->nativeMessage = {};
    app_.viewports.clear();
    producer_.request_stop();
    sampler_.request_stop();
    if (producer_.joinable()) producer_.join();
    if (sampler_.joinable()) sampler_.join();
#ifdef Q_OS_WIN
    if (predecessorHandle_) CloseHandle(predecessorHandle_);
#endif
    for (auto* tab : documents_) {
      tab->session.document().setModified(false);
      DraftRecovery().markClean(tab->session.filePath(), tab->draftKey);
    }
  }
  void start() {
    setupStart_ = nowNs();
    window_ = std::make_unique<MainWindow>();
    // Optional for an interactive desktop with other applications: keep only
    // this disposable benchmark window visible. Activation checks still apply.
    if (qEnvironmentVariableIsSet("MUFFIN_GUI_PIN_WINDOW")) window_->setWindowFlag(Qt::WindowStaysOnTopHint);
    tabs_ = window_->findChild<QTabWidget*>("DocumentTabs");
    check(tabs_, "document tab widget missing");
    window_->resize(1100, 820);
    window_->show();
    window_->raise();
    window_->activateWindow();
    connect(window_->windowHandle(), &QWindow::activeChanged, this, [this] {
      if (!recording_) return;
      if (!window_->windowHandle()->isActive()) {
        lostActivation_ = true;
        observeForegroundLoss("activation_event");
      }
      traceRow({{"kind", "window_activation"},
                {"at_ms", ms(nowNs() - traceStart)},
                {"active", window_->windowHandle()->isActive()}});
    });
    heartbeat_.setTimerType(Qt::PreciseTimer);
    heartbeat_.setInterval(8);
    connect(&heartbeat_, &QTimer::timeout, this, [this] {
      const auto now = nowNs();
      if (recording_ && lastBeat_) {
        lostActivation_ |= !window_->isActiveWindow();
        lostExposure_ |= !window_->windowHandle()->isExposed();
        lostForeground_ |= !hasWindowsForeground();
        if (!hasWindowsForeground()) observeForegroundLoss("heartbeat");
        const auto delay = qMax(0.0, ms(now - lastBeat_) - 8);
        heartbeatDelays_.append(delay);
        if (activeTrace && delay >= 16)
          traceRow({{"kind", "heartbeat"},
                    {"start_ms", ms(lastBeat_ - traceStart)},
                    {"end_ms", ms(now - traceStart)},
                    {"delay_ms", delay}});
      }
      lastBeat_ = now;
    });
    heartbeat_.start();
    sampler_ = std::jthread([this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        if (recordingMemory_.load()) {
          if (queueTrace && qEnvironmentVariableIsSet("MUFFIN_GUI_TRACE")) {
            const auto begin = nowNs();
            auto row = qEnvironmentVariableIsSet("MUFFIN_GUI_SCHEDULER_SAMPLES") ? queueTrace->schedulingSample()
                                                                                 : queueTrace->threadSample();
            row["query_begin_ns"] = begin;
            row["at_ns"] = nowNs();
            std::lock_guard lock(schedulerMutex_);
            schedulerSamples_.append(row);
          }
          const auto value = diag::workingSetBytes();
          auto prior = sampledMaxMemory_.load();
          while (value > prior && !sampledMaxMemory_.compare_exchange_weak(prior, value)) {
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
    app_.painted = [this](int tab, double elapsed, bool full) { onPaint(tab, elapsed, full); };
    app_.activeTab = [this] { return tabs_->currentIndex(); };
    app_.hiddenEvent = [this](int tab, int type, double elapsed) {
      if (!recording_) return;
      const auto key = QString::number(tab) + '/' + QString::number(type);
      ++hiddenEvents_[key].first;
      hiddenEvents_[key].second += elapsed;
    };
    connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, this,
            [] { traceRow({{"kind", "dispatcher_block"}, {"at_ms", ms(nowNs() - traceStart)}}); });
    connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::awake, this,
            [] { traceRow({{"kind", "dispatcher_awake"}, {"at_ms", ms(nowNs() - traceStart)}}); });
    if (queueTrace)
      queueTrace->nativeMessage = [](QJsonObject row) {
        row["kind"] = "native_message";
        row["at_ms"] = ms(nowNs() - traceStart);
        traceRow(row);
      };
    app_.eventCompleted = [](const QString& target, int type, int interval, qint64 start, qint64 end,
                             QJsonObject cpuBefore, QJsonObject cpuAfter) {
      if (activeTrace)
        traceRow({{"kind", "event"},
                  {"target", target},
                  {"type", type},
                  {"timer_interval_ms", interval},
                  {"start_ms", ms(start - traceStart)},
                  {"end_ms", ms(end - traceStart)},
                  {"duration_ms", ms(end - start)},
                  {"cpu_before", cpuBefore},
                  {"cpu_after", cpuAfter},
                  {"operation", traceOperation}});
    };
    app_.beforeNativeKey = [this](QObject* target, QKeyEvent* key) {
      if (!recording_ || !nativeInput_) return;
      if (nativeObservations_.size() < 12)
        nativeObservations_.append(QJsonObject{{"target", target->metaObject()->className()},
                                               {"name", target->objectName()},
                                               {"key", key->key()},
                                               {"text", key->text()},
                                               {"expected", target == current()->renderView}});
      if (qobject_cast<QWindow*>(target)) return;
      if (target != current()->renderView || key->key() != Qt::Key_X || key->text() != "x" || nativeCurrent_ >= 0) {
        nativeContaminated_ = true;
        return;
      }
      std::lock_guard lock(nativeMutex_);
      if (nativeReceived_ >= nativePosts_.size()) {
        nativeContaminated_ = true;
        return;
      }
      nativeCurrent_ = nativeReceived_++;
      const auto [due, posted] = nativePosts_[nativeCurrent_];
      beginOperation(nativeCurrent_, due, posted);
      samples_[nativeCurrent_].detail = "x/native";
    };
    app_.afterNativeKey = [this] {
      if (nativeCurrent_ < 0) return;
      endOperation(nativeCurrent_);
      nativeCurrent_ = -1;
    };
    waitForPredecessor();
  }

 protected:
  bool event(QEvent* event) override {
    if (event->type() != operationType) return QObject::event(event);
    auto* operation = static_cast<OperationEvent*>(event);
    if (operation->index < 0) {
      producerDone_ = true;
      QTimer::singleShot(500, this, [this] { finishPhase(); });
      return true;
    }
    beginOperation(operation->index, operation->due, operation->posted);
    dispatch(operation->index, samples_[operation->index]);
    endOperation(operation->index);
    return true;
  }

 private:
  void writeChainState(const QString& status, qint64 successor = 0, bool granted = false,
                       QJsonObject handoff = {}) const {
    if (chainFile_.isEmpty()) return;
    const auto document = QJsonDocument(QJsonObject{{"status", status},
                                                    {"index", chainIndex_},
                                                    {"pid", QCoreApplication::applicationPid()},
                                                    {"successor_pid", successor},
                                                    {"foreground_granted", granted},
                                                    {"handoff", handoff},
                                                    {"output", output_}})
                              .toJson();
    QSaveFile ownState(output_ + ".chain.json");
    check(ownState.open(QIODevice::WriteOnly), "cannot write own GUI chain state");
    ownState.write(document);
    check(ownState.commit(), "cannot commit own GUI chain state");
    QSaveFile state(chainFile_ + ".state.json");
    check(state.open(QIODevice::WriteOnly), "cannot write GUI chain state");
    state.write(document);
    check(state.commit(), "cannot commit GUI chain state");
  }
  void waitForPredecessor() {
#ifdef Q_OS_WIN
    if (predecessorHandle_) {
      const auto waited = WaitForSingleObject(predecessorHandle_, 0);
      if (waited == WAIT_TIMEOUT) {
        QTimer::singleShot(20, this, [this] { waitForPredecessor(); });
        return;
      }
      check(waited == WAIT_OBJECT_0, "predecessor exit wait failed");
      CloseHandle(predecessorHandle_);
      predecessorHandle_ = nullptr;
      // The preceding foreground benchmark granted this exact process ordinary
      // Windows activation permission. Do this before any timed operation.
      window_->raise();
      window_->activateWindow();
      window_->windowHandle()->requestActivate();
    }
#endif
    writeChainState("running");
    openNext();
  }
  void handOffChain(bool valid) {
    if (chainFile_.isEmpty()) return;
    if (!valid) {
      writeChainState("failed_validation");
      return;
    }
    const auto jobs = chainPlan_["jobs"].toArray();
    if (chainIndex_ + 1 == jobs.size()) {
      writeChainState("completed");
      return;
    }
    const auto next = jobs[chainIndex_ + 1].toObject();
    QProcess successor;
    successor.setProgram(QCoreApplication::applicationFilePath());
    successor.setArguments({next["output"].toString(), QString::number(next["mib"].toInt()),
                            QString::number(next["tabs"].toInt()), QString::number(next["cycles"].toInt())});
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("MUFFIN_GUI_CHAIN_INDEX", QString::number(chainIndex_ + 1));
    environment.insert("MUFFIN_GUI_PREDECESSOR_PID", QString::number(QCoreApplication::applicationPid()));
    environment.insert("MUFFIN_GUI_PROFILE", next["profile"].toString());
    if (next.contains("open_profile")) environment.insert("MUFFIN_GUI_OPEN_PROFILE", next["open_profile"].toString());
    successor.setProcessEnvironment(environment);
    successor.setWorkingDirectory(QDir::currentPath());
    successor.setProcessChannelMode(QProcess::MergedChannels);
    successor.setStandardOutputFile(next["output"].toString() + ".log");
    const auto origin = foregroundIdentity(true);
    qint64 pid = 0;
    check(successor.startDetached(&pid), "cannot start successor GUI benchmark");
    bool granted = false;
    int grantError = 0;
#ifdef Q_OS_WIN
    SetLastError(0);
    granted = AllowSetForegroundWindow(DWORD(pid));
    if (!granted) grantError = int(GetLastError());
#endif
    writeChainState("handed_off", pid, granted,
                    {{"origin", origin}, {"after_launch", foregroundIdentity(true)}, {"grant_error", grantError}});
    // A process started by the foreground process can also activate normally.
    // A failed explicit grant is retained; it is never treated as proof of
    // foreground. Only the next process's actual foreground checks decide that.
  }
  bool hasWindowsForeground() const {
#ifdef Q_OS_WIN
    return GetForegroundWindow() == reinterpret_cast<HWND>(window_->effectiveWinId());
#else
    return false;
#endif
  }
  void observeForegroundLoss(const QString& context) {
    if (!foregroundLoss_.isEmpty()) return;
    foregroundLoss_ = {{"context", context},
                       {"at_ms", ms(nowNs() - phaseStart_)},
                       {"win32", foregroundIdentity(true)},
                       {"desktop", queueTrace ? queueTrace->desktopIdentity() : QJsonObject{}}};
  }
  QJsonObject foregroundIdentity(bool diagnostic = false) const {
    QJsonObject result;
#ifdef Q_OS_WIN
    const auto expected = reinterpret_cast<HWND>(window_->effectiveWinId());
    const auto foreground = GetForegroundWindow();
    DWORD process = 0;
    const auto thread = foreground ? GetWindowThreadProcessId(foreground, &process) : 0;
    result = {{"expected_hwnd", QString::number(quintptr(expected))},
              {"expected_root_hwnd", QString::number(quintptr(GetAncestor(expected, GA_ROOT)))},
              {"foreground_hwnd", QString::number(quintptr(foreground))},
              {"foreground_process", qint64(process)},
              {"foreground_thread", qint64(thread)},
              {"own_process", qint64(GetCurrentProcessId())},
              {"gui_thread", qint64(GetCurrentThreadId())},
              {"expected_is_window", bool(IsWindow(expected))},
              {"expected_visible", bool(IsWindowVisible(expected))},
              {"expected_iconic", bool(IsIconic(expected))},
              { "foreground_is_own_process",
                process == GetCurrentProcessId() }};
    GUITHREADINFO own{sizeof(GUITHREADINFO)};
    if (GetGUIThreadInfo(GetCurrentThreadId(), &own)) {
      result["thread_active_hwnd"] = QString::number(quintptr(own.hwndActive));
      result["thread_focus_hwnd"] = QString::number(quintptr(own.hwndFocus));
    }
    if (diagnostic && foreground) {
      wchar_t name[4096]{};
      if (GetClassNameW(foreground, name, 4096)) result["foreground_class"] = QString::fromWCharArray(name);
      if (const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process)) {
        DWORD length = 4096;
        if (QueryFullProcessImageNameW(handle, 0, name, &length))
          result["foreground_executable"] = QString::fromWCharArray(name, int(length));
        CloseHandle(handle);
      }
    }
#endif
    return result;
  }
  void writeForegroundReadiness(const QString& status) const {
    QFile readinessFile(output_ + ".ready.json");
    check(readinessFile.open(QIODevice::WriteOnly), "cannot write foreground readiness record");
    readinessFile.write(QJsonDocument(QJsonObject{{"status", status},
                                                  {"result", output_},
                                                  {"window_title", window_->windowTitle()},
                                                  {"wait_limit_ms", foregroundWaitLimitMs_},
                                                  {"win32", foregroundIdentity()}})
                            .toJson());
  }
  void beginOperation(int index, qint64 due, qint64 posted) {
    auto& sample = samples_[index];
    sample.due = due;
    sample.posted = posted;
    sample.dispatch = nowNs();
    if (themeAcceptance_) sample.stagesBefore = themePerformanceSamples();
    if (activeTrace && queueTrace) sample.cpuBefore = queueTrace->threadSample();
    duringOperationPaint_.clear();
    duringOperationPaintStart_.clear();
    inOperation_ = true;
    traceOperation = index;
    beforeTab_ = current();
    beforeSelection_ = beforeTab_->editorController.selection().selection();
    beforeRevision_ = beforeTab_->session.document().revision();
    beforeScroll_ = beforeTab_->renderView->verticalScrollBar()->value();
  }
  void endOperation(int index) {
    auto& sample = samples_[index];
    const auto after = current()->editorController.selection().selection();
    sample.stateChanged = current() != beforeTab_ || beforeRevision_ != current()->session.document().revision() ||
                          beforeScroll_ != current()->renderView->verticalScrollBar()->value() ||
                          !samePosition(beforeSelection_.anchor, after.anchor) ||
                          !samePosition(beforeSelection_.focus, after.focus);
    traceOperation = -1;
    inOperation_ = false;
    sample.tab = tabs_->currentIndex();
    sample.end = nowNs();
    if (activeTrace && queueTrace) sample.cpuAfter = queueTrace->threadSample();
    sample.paint = duringOperationPaint_.value(sample.tab);
    sample.paintStart = duringOperationPaintStart_.value(sample.tab);
  }
  DocumentTab* current() const { return static_cast<DocumentTab*>(tabs_->currentWidget()); }
  void openNext() {
    if (documents_.size() == tabCount_) {
      configurePerformanceLogging(false);
      tabs_->setCurrentIndex(0);
      QTimer::singleShot(1500, this, [this] { beginMeasurements(); });
      return;
    }
    // A manual activation can happen long after startup. Gate before creating
    // the workload and starting the open clock, rather than only before input
    // phases: otherwise the first paint was already recorded in the background.
    if (qEnvironmentVariableIsSet("MUFFIN_GUI_REQUIRE_FOREGROUND")) {
      const auto now = nowNs();
      if (!openForegroundWaitStart_) {
        openForegroundWaitStart_ = now;
        current()->renderView->setFocus(Qt::OtherFocusReason);
        window_->setWindowTitle("Muffin theme benchmark | click editor to start document opening");
        writeForegroundReadiness("waiting_for_foreground");
      }
      const bool ready = hasWindowsForeground() && window_->isActiveWindow() && window_->windowHandle()->isExposed() &&
                         QApplication::focusWidget() == current()->renderView;
      if (!ready) openForegroundStableStart_ = 0;
      if (ready && !openForegroundStableStart_) openForegroundStableStart_ = now;
      if (!ready || ms(now - openForegroundStableStart_) < 500) {
        if (ms(now - openForegroundWaitStart_) < foregroundWaitLimitMs_) {
          QTimer::singleShot(100, this, [this] { openNext(); });
        } else {
          writeForegroundReadiness("foreground_timeout_before_open");
          writeChainState("failed_foreground_before_open");
          std::fprintf(stderr, "GUI foreground wait timed out before document opening\n");
          app_.exit(1);
        }
        return;
      }
      openForegroundWaits_.append(ms(now - openForegroundWaitStart_));
      openForegroundWaitStart_ = openForegroundStableStart_ = 0;
      window_->setWindowTitle("Muffin theme benchmark | opening; keep editor in foreground");
      writeForegroundReadiness("opening");
    }
    const auto path = files_.filePath(QString("gui-%1.md").arg(documents_.size() + 1));
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "cannot create workload");
    const auto text = workload(qsizetype(mib_ * 1024 * 1024)).toUtf8();
    check(file.write(text) == text.size(), "workload write failed");
    file.close();
    configurePerformanceLogging(true);
    {
      std::lock_guard lock(openProbeMutex);
      openProbeScopes = {};
    }
    resetThemePerformance();
    sampledMaxMemory_ = diag::workingSetBytes();
    recordingMemory_ = true;
    const auto started = nowNs();
    openProbeStart = started;
    openFirstPaint_ = openInstallFinished_ = 0;
    check(window_->openFile(path), "file open failed");
    openCallReturned_ = nowNs();
    auto* tab = current();
    documents_.append(tab);
    tab->draftTimer->setObjectName(QString("benchmarkDraft/%1").arg(documents_.size() - 1));
    if (qEnvironmentVariableIsSet("MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT")) tab->draftTimer->blockSignals(true);
    app_.viewports.insert(tab->renderView->viewport(), documents_.size() - 1);
    connect(&tab->session, &DocumentSession::parseBusy, this, [this](bool busy) {
      if (!busy && openProbeStart.load()) openInstallFinished_ = nowNs();
    });
    waitForOpen(tab, started);
  }
  void waitForOpen(DocumentTab* tab, qint64 started) {
    check(ms(nowNs() - started) < 180000, "file load timed out");
    if (tab->session.isAsyncParseInProgress() || !openFirstPaint_) {
      QTimer::singleShot(20, this, [this, tab, started] { waitForOpen(tab, started); });
      return;
    }
    recordingMemory_ = false;
    QJsonArray scopes;
    {
      std::lock_guard lock(openProbeMutex);
      scopes = openProbeScopes;
    }
    QJsonObject stages;
    const char* names[]{"css", "counters", "layout", "paint", "background", "shadow", "text"};
    const auto samples = themePerformanceSamples();
    for (size_t i = 0; i < samples.size(); ++i)
      stages[names[i]] = QJsonObject{{"exclusive_ms", samples[i].exclusiveNs / 1e6},
                                     {"calls", qint64(samples[i].calls)}, {"work", qint64(samples[i].work)}};
    openRows_.append(QJsonObject{{"tab", documents_.size() - 1},
                                 {"parse_and_bind_ms", ms(openInstallFinished_ - started)},
                                 {"call_return_ms", ms(openCallReturned_ - started)},
                                 {"install_finished_ms", ms(openInstallFinished_ - started)},
                                 {"first_full_paint_ms", ms(openFirstPaint_ - started)},
                                 {"open_profile", qEnvironmentVariable("MUFFIN_GUI_OPEN_PROFILE", "legacy")},
                                 {"sampled_max_working_set", sampledMaxMemory_.load()},
                                 {"foreground_at_first_paint", openPaintForeground_},
                                 {"stages", stages}, {"scopes", scopes},
                                 {"working_set_bytes", diag::workingSetBytes()},
                                 {"process", diag::MemoryAccounting::process()},
                                 {"blocks", qint64(tab->session.document().root().children().size())}});
    openProbeStart = 0;
    QTimer::singleShot(1000, this, [this] { openNext(); });
  }
  void resetCaret() {
    auto* tab = current();
    auto* paragraph = tab->session.document().root().children()[1].get();
    CursorPosition caret;
    caret.blockId = caret.text.nodeId = paragraph->id();
    caret.text.textOffset = themeAcceptance_ ? 5 : 40;
    caret.text.sourceOffset = paragraph->sourceRange().byteStart + caret.text.textOffset;
    tab->editorController.selection().setCursorPosition(caret);
    tab->renderView->setFocus();
    tab->renderView->verticalScrollBar()->setValue(0);
  }
  void beginMeasurements(int activationAttempt = 0) {
    if (!activationAttempt && qEnvironmentVariableIsSet("MUFFIN_GUI_REQUIRE_FOREGROUND")) {
      foregroundBeforeActivation_ = foregroundIdentity();
      // Loading can outlast the initial request. Qt's active flag alone is not
      // enough on Windows: request the ordinary activation once after setup.
      if (!hasWindowsForeground()) {
        window_->raise();
        window_->activateWindow();
        if (window_->windowHandle()) window_->windowHandle()->requestActivate();
      }
    }
    // Opening several files can outlast Windows' initial activation request.
    // Reacquire focus and let native events settle before recording anything.
    if (!window_->windowHandle() || !window_->windowHandle()->isExposed() || !window_->isActiveWindow()) {
      check(activationAttempt < 10, "benchmark window must be exposed and active");
      window_->raise();
      window_->activateWindow();
      if (window_->windowHandle()) window_->windowHandle()->requestActivate();
      QTimer::singleShot(500, this, [this, activationAttempt] { beginMeasurements(activationAttempt + 1); });
      return;
    }
    resetCaret();
    phases_ = {{"typing", count_, 50}, {"cursor_horizontal", count_, 50}, {"cursor_vertical", count_, 50}};
    if (nativeInput_ || qEnvironmentVariableIsSet("MUFFIN_GUI_TYPING_ONLY")) phases_ = {{"typing", count_, 50}};
    // A focused queue reproduction keeps the same input prefix, without waiting
    // through unrelated scroll/theme phases on an interactive desktop.
    if (!qEnvironmentVariableIsSet("MUFFIN_GUI_INPUT_ONLY") && !qEnvironmentVariableIsSet("MUFFIN_GUI_TYPING_ONLY") &&
        !nativeInput_) {
      phases_.append({"scroll_cold", count_, 100});
      phases_.append({"scroll_warm", count_, 100});
      phases_.append({"theme_switch", 12, 1500});
      if (tabCount_ > 1) {
        phases_.append({"tab_switch_deferred_theme", tabCount_ - 1, 1500});
        phases_.append({"tab_switch_warm", count_, 100});
        phases_.append({"typing_multiple_tabs", count_, 50});
      }
    }
    if (themeAcceptance_) {
      phases_ = {{"theme_switch", count_ * 3, 1500}, {"typing_after_theme", 8, 100}};
      if (tabCount_ > 1) {
        phases_.append({"tab_switch_deferred_theme", tabCount_ - 1, 1500});
        phases_.append({"typing_multiple_tabs", 8, 100});
        phases_.append({"tab_switch_warm", 8, 100});
      }
    }
    setupMs_ = ms(nowNs() - setupStart_);
    QTimer::singleShot(1000, this, [this] { nextPhase(); });
  }
  void triggerTheme(const QString& id) {
    for (auto* action : window_->findChildren<QAction*>())
      if (action->isCheckable() && action->data().toString() == id) {
        action->trigger();
        return;
      }
    qFatal("GUI probe: missing theme action %s", qPrintable(id));
  }
  void sendKey(int key, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QApplication::sendEvent(current()->renderView, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QApplication::sendEvent(current()->renderView, &release);
  }
  void dispatch(int index, Sample& sample) {
    const auto& name = phases_[phase_].name;
    if (name.startsWith("typing")) {
      sendKey(Qt::Key_X, "x");
      sample.detail = "x";
    } else if (name == "cursor_horizontal") {
      const auto key = index % 20 < 10 ? Qt::Key_Right : Qt::Key_Left;
      sendKey(key);
      sample.detail = key == Qt::Key_Right ? "right" : "left";
    } else if (name == "cursor_vertical") {
      const auto key = index % 12 < 6 ? Qt::Key_Down : Qt::Key_Up;
      sendKey(key);
      sample.detail = key == Qt::Key_Down ? "down" : "up";
    } else if (name.startsWith("scroll")) {
      auto* viewport = current()->renderView->viewport();
      const QPointF pos = viewport->rect().center();
      // Return over the cold phase's already visited region; no new range is
      // introduced by the warm phase's first ten events.
      const int delta = name == "scroll_cold" || index % 20 >= 10 ? -120 : 120;
      QWheelEvent wheel(pos, viewport->mapToGlobal(pos.toPoint()), {}, {0, delta}, Qt::NoButton, Qt::NoModifier,
                        Qt::NoScrollPhase, false);
      QApplication::sendEvent(viewport, &wheel);
      sample.detail = QString::number(delta);
    } else if (name == "theme_switch") {
      const QString targets[]{"gui-vlook", "github", "gui-phycat"};
      sample.detail = themeAcceptance_ ? targets[index % 3] : index % 2 == 0 ? "github" : "gui-vlook";
      triggerTheme(sample.detail);
      if (themeAcceptance_) {
        const auto after = current()->editorController.selection().selection();
        check(current()->session.document().revision() == beforeRevision_ &&
                  samePosition(after.anchor, beforeSelection_.anchor) &&
                  samePosition(after.focus, beforeSelection_.focus),
              "theme switch changed document or selection");
      }
      for (int i = 1; i < documents_.size(); ++i) {
        auto* hidden = documents_[i];
        check(hidden->pendingTheme.has_value(), "hidden theme must remain deferred");
        check(hidden->renderView->theme().documentStyleSheet() == hiddenSheets_[i] &&
                  hidden->renderView->layoutTotalHeight() == hiddenHeights_[i] &&
                  hidden->session.document().revision() == hiddenRevisions_[i],
              "hidden layout changed on theme switch");
      }
    } else if (name.startsWith("tab_switch")) {
      const int next = name == "tab_switch_deferred_theme" ? index + 1 : (tabs_->currentIndex() + 1) % tabCount_;
      const auto point = tabs_->tabBar()->tabRect(next).center();
      QMouseEvent press(QEvent::MouseButtonPress, point, tabs_->tabBar()->mapToGlobal(point), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(tabs_->tabBar(), &press);
      QMouseEvent release(QEvent::MouseButtonRelease, point, tabs_->tabBar()->mapToGlobal(point), Qt::LeftButton,
                          Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(tabs_->tabBar(), &release);
      check(tabs_->currentIndex() == next && !current()->pendingTheme,
            "tab activation did not apply its deferred theme");
      sample.detail = QString::number(next);
    }
  }
  void nextPhase() {
    if (++phase_ == phases_.size()) {
      finish();
      return;
    }
    const auto& phase = phases_[phase_];
    std::fprintf(stderr, "GUI %.0f MiB/%d tabs: %s (%d at %d ms)\n", mib_, tabCount_, qPrintable(phase.name),
                 phase.count, phase.intervalMs);
    std::fflush(stderr);
    if (phase.name.startsWith("typing") || phase.name.startsWith("cursor")) resetCaret();
    if (phase.name == "scroll_cold") current()->renderView->verticalScrollBar()->setValue(0);
    if (phase.name == "theme_switch") {
      tabs_->setCurrentIndex(0);
      if (themeAcceptance_) resetCaret();
      for (auto* tab : documents_) {
        hiddenSheets_.append(tab->renderView->theme().documentStyleSheet());
        hiddenHeights_.append(tab->renderView->layoutTotalHeight());
        hiddenRevisions_.append(tab->session.document().revision());
      }
    }
    QTimer::singleShot(500, this, [this] { launchPhase(); });
  }
  void launchPhase() {
    // A visible Qt window can still be behind another Windows foreground window.
    // Wait outside the measured interval; never steal focus while posting input.
    if (qEnvironmentVariableIsSet("MUFFIN_GUI_REQUIRE_FOREGROUND")) {
      const auto now = nowNs();
      if (!foregroundWaitStart_) foregroundWaitStart_ = now;
      const bool ready = hasWindowsForeground() && window_->isActiveWindow() && window_->windowHandle()->isExposed() &&
                         QApplication::focusWidget() == current()->renderView;
      if (!ready) foregroundStableStart_ = 0;
      if (ready && !foregroundStableStart_) foregroundStableStart_ = now;
      if (!ready || ms(now - foregroundStableStart_) < 500) {
        if (ms(now - foregroundWaitStart_) < foregroundWaitLimitMs_) {
          if (!foregroundWaitAnnounced_) {
            foregroundWaitAnnounced_ = true;
            window_->setWindowTitle(
                QString(themeAcceptance_ ? "Muffin theme benchmark | %1 / %2 | click editor to start %3 operations"
                                         : "Muffin input benchmark | %1 / %2 | click editor to start %3 keys")
                    .arg(nativeInput_ ? "Windows" : "Qt", qEnvironmentVariable("MUFFIN_GUI_PROFILE"))
                    .arg(phases_[phase_].count));
            writeForegroundReadiness("waiting_for_foreground");
            std::fprintf(stderr, "GUI waiting for benchmark foreground and editor focus (up to %d seconds)\n",
                         foregroundWaitLimitMs_ / 1000);
            std::fflush(stderr);
          }
          QTimer::singleShot(100, this, [this] { launchPhase(); });
          return;
        }
        foregroundWaitTimedOut_ = true;
        foregroundWaitFailure_ = {{"qt_active", window_->isActiveWindow()},
                                  {"exposed", window_->windowHandle()->isExposed()},
                                  {"editor_focused", QApplication::focusWidget() == current()->renderView},
                                  {"windows_foreground", hasWindowsForeground()},
                                  {"win32", foregroundIdentity()}};
        writeForegroundReadiness("foreground_timeout");
        finish();
        return;
      }
      foregroundWaits_.append(ms(now - foregroundWaitStart_));
      foregroundWaitStart_ = foregroundStableStart_ = 0;
      foregroundWaitAnnounced_ = false;
      window_->setWindowTitle(themeAcceptance_ ? "Muffin theme benchmark | recording; keep editor in foreground"
                                               : "Muffin input benchmark | recording; keep editor in foreground");
      writeForegroundReadiness("recording");
    }
    samples_.clear();
    samples_.resize(phases_[phase_].count);
    paints_.clear();
    hiddenEvents_.clear();
    app_.editorObjects.clear();
    for (int i = 0; i < documents_.size(); ++i) {
      auto* view = documents_[i]->renderView;
      app_.editorObjects.insert(view, i);
      for (auto* object : view->findChildren<QObject*>()) app_.editorObjects.insert(object, i);
    }
    heartbeatDelays_.clear();
    trace_ = {};
    themeScopes_ = {};
    probes_.clear();
    paintsByTab_.fill(0, tabCount_);
    hiddenPaints_ = 0;
    lostActivation_ = !window_->isActiveWindow();
    lostExposure_ = !window_->windowHandle()->isExposed();
    lostForeground_ = !hasWindowsForeground();
    foregroundLoss_ = {};
    memoryBefore_ = diag::workingSetBytes();
    phaseRevisionBefore_ = current()->session.document().revision();
    sampledMaxMemory_ = memoryBefore_;
    recordingMemory_ = true;
    recording_ = true;
    producerDone_ = false;
    lastBeat_ = nowNs();
    phaseStart_ = nowNs();
    traceStart = phaseStart_;
    traceDropped = 0;
    traceViewports.clear();
    for (auto it = app_.viewports.cbegin(); it != app_.viewports.cend(); ++it)
      traceViewports.insert(quintptr(it.key()), it.value());
    {
      std::lock_guard lock(schedulerMutex_);
      schedulerSamples_.clear();
    }
    activeTrace = qEnvironmentVariableIsSet("MUFFIN_GUI_TRACE") ? &trace_ : nullptr;
    app_.traceEvents = activeTrace != nullptr;
    resetThemePerformance();
    activeProbes = &probes_;
    themeScopeIntervals =
        themeAcceptance_ && qEnvironmentVariable("MUFFIN_GUI_PROFILE") != "off" ? &themeScopes_ : nullptr;
    const auto phase = phases_[phase_];
#ifdef Q_OS_WIN
    const auto inputWindow = reinterpret_cast<HWND>(window_->winId());
#endif
    producer_ = std::jthread([this, phase
#ifdef Q_OS_WIN
                              ,
                              inputWindow
#endif
    ](std::stop_token stop) {
      const auto start = Clock::now() + std::chrono::milliseconds(30);
      for (int i = 0; i < phase.count && !stop.stop_requested(); ++i) {
        const auto due = start + std::chrono::milliseconds(i * phase.intervalMs);
        std::this_thread::sleep_until(due);
        if (nativeInput_) {
#ifdef Q_OS_WIN
          // Never send into another application after a foreground change.
          if (GetForegroundWindow() != inputWindow) {
            nativeForegroundMismatch_ = quintptr(GetForegroundWindow());
            nativeExpectedWindow_ = quintptr(inputWindow);
            nativeAborted_ = true;
            break;
          }
          const auto dueNs = std::chrono::duration_cast<std::chrono::nanoseconds>(due.time_since_epoch()).count();
          {
            std::lock_guard lock(nativeMutex_);
            nativePosts_.append({dueNs, nowNs()});
          }
          INPUT keys[2]{};
          keys[0].type = keys[1].type = INPUT_KEYBOARD;
          keys[0].ki.wVk = keys[1].ki.wVk = 'X';
          keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
          if (SendInput(2, keys, sizeof(INPUT)) != 2) {
            nativeAborted_ = true;
            break;
          }
#endif
        } else
          QCoreApplication::postEvent(
              this, new OperationEvent(
                        i, std::chrono::duration_cast<std::chrono::nanoseconds>(due.time_since_epoch()).count()));
      }
      QCoreApplication::postEvent(this, new OperationEvent(-1, nowNs()));
    });
  }
  void onPaint(int tab, double elapsed, bool fullViewportPaint) {
    if (openProbeStart.load() && openInstallFinished_ && !openFirstPaint_ && fullViewportPaint && tab == documents_.size() - 1 &&
        !current()->session.isAsyncParseInProgress() && !current()->session.document().root().children().empty()) {
      openFirstPaint_ = nowNs();
      openPaintForeground_ = hasWindowsForeground() && window_->isActiveWindow() && window_->windowHandle()->isExposed();
    }
    if (!recording_) return;
    paints_.append(elapsed);
    ++paintsByTab_[tab];
    if (tab != tabs_->currentIndex()) ++hiddenPaints_;
    // A caret-only dirty rectangle does not establish that the new theme's
    // entire visible viewport has been rendered.
    if (themeAcceptance_ && phases_[phase_].name == "theme_switch" && !fullViewportPaint) return;
    const auto completed = nowNs();
    const auto started = completed - qint64(elapsed * 1e6);
    if (inOperation_) {
      duringOperationPaint_[tab] = completed;
      duringOperationPaintStart_[tab] = started;
      if (themeAcceptance_ && traceOperation >= 0) {
        samples_[traceOperation].stagesPainted = themePerformanceSamples();
        samples_[traceOperation].fullViewportPaint = fullViewportPaint;
      }
    }
    for (auto& sample : samples_)
      if (sample.end && !sample.paint && sample.tab == tab) {
        sample.paint = completed;
        sample.paintStart = started;
        if (themeAcceptance_) sample.stagesPainted = themePerformanceSamples();
        sample.fullViewportPaint = fullViewportPaint;
      }
  }
  void finishPhase() {
    if (!producerDone_) return;
    activeProbes = nullptr;
    activeTrace = nullptr;
    app_.traceEvents = false;
    themeScopeIntervals = nullptr;
    recording_ = false;
    recordingMemory_ = false;
    producer_.join();
    QVector<double> sync, queue, paint, dispatchPaint, postJitter;
    QJsonArray raw, paintsByTab;
    int coalesced = 0;
    qint64 previousPaint = 0;
    for (const auto& sample : samples_) {
      if (!sample.dispatch) continue;
      sync.append(ms(sample.end - sample.dispatch));
      queue.append(ms(sample.dispatch - sample.due));
      postJitter.append(ms(sample.posted - sample.due));
      QJsonObject row{{"dispatch_ms", ms(sample.dispatch - sample.due)},
                      {"sync_ms", ms(sample.end - sample.dispatch)},
                      {"post_jitter_ms", ms(sample.posted - sample.due)},
                      {"tab", sample.tab},
                      {"detail", sample.detail}};
      row["editor_state_changed"] = sample.stateChanged;
      row["first_paint_full_viewport"] = sample.fullViewportPaint;
      // Retain the same clock edges in every mode. This is serialized after the
      // phase, without per-keystroke trace allocation or Windows queue polling.
      row["due_at_ms"] = ms(sample.due - phaseStart_);
      row["posted_at_ms"] = ms(sample.posted - phaseStart_);
      row["dispatch_at_ms"] = ms(sample.dispatch - phaseStart_);
      row["end_at_ms"] = ms(sample.end - phaseStart_);
      if (sample.paint) row["paint_at_ms"] = ms(sample.paint - phaseStart_);
      if (sample.paintStart) row["paint_start_at_ms"] = ms(sample.paintStart - phaseStart_);
      int pendingAtDispatch = 0, pendingAfterHandler = 0, subsequentBeforePaint = 0;
      for (const auto& later : samples_) {
        if (!later.dispatch || later.dispatch <= sample.dispatch) continue;
        pendingAtDispatch += later.posted <= sample.dispatch;
        pendingAfterHandler += later.posted <= sample.end && later.dispatch >= sample.end;
        subsequentBeforePaint += sample.paintStart && later.dispatch < sample.paintStart;
      }
      row["pending_inputs_at_dispatch"] = pendingAtDispatch;
      row["pending_inputs_after_handler"] = pendingAfterHandler;
      row["subsequent_inputs_before_paint"] = subsequentBeforePaint;
      row["post_to_dispatch_ms"] = ms(sample.dispatch - sample.posted);
      if (sample.paintStart) row["handler_end_to_paint_start_ms"] = qMax(0.0, ms(sample.paintStart - sample.end));
      if (qEnvironmentVariableIsSet("MUFFIN_GUI_TRACE")) {
        row["cpu_before"] = sample.cpuBefore;
        row["cpu_after"] = sample.cpuAfter;
      }
      if (sample.paint) {
        if (themeAcceptance_) {
          const char* stageNames[]{"css", "counters", "layout", "paint", "background", "shadow", "text"};
          QJsonObject stages;
          for (size_t i = 0; i < sample.stagesBefore.size(); ++i)
            stages[stageNames[i]] = (sample.stagesPainted[i].exclusiveNs - sample.stagesBefore[i].exclusiveNs) / 1e6;
          row["stages_to_first_paint_ms"] = stages;
        }
        paint.append(ms(sample.paint - sample.due));
        dispatchPaint.append(ms(sample.paint - sample.dispatch));
        row["scheduled_to_paint_ms"] = ms(sample.paint - sample.due);
        row["dispatch_to_paint_ms"] = ms(sample.paint - sample.dispatch);
        if (sample.paint == previousPaint) ++coalesced;
        previousPaint = sample.paint;
      } else
        row["paint_observed"] = false;
      raw.append(row);
    }
    for (const auto count : paintsByTab_) paintsByTab.append(count);
    QJsonObject scopeRows;
    for (auto it = probes_.cbegin(); it != probes_.cend(); ++it) scopeRows[it.key()] = distribution(it.value());
    QJsonObject stages;
    QJsonObject hiddenEvents;
    for (auto it = hiddenEvents_.cbegin(); it != hiddenEvents_.cend(); ++it)
      hiddenEvents[it.key()] = QJsonObject{{"calls", it->first}, {"handler_ms", it->second}};
    const char* names[]{"css", "counters", "layout", "paint", "background", "shadow", "text"};
    const auto scopes = themePerformanceSamples();
    for (size_t i = 0; i < scopes.size(); ++i)
      stages[names[i]] = QJsonObject{{"exclusive_ms", scopes[i].exclusiveNs / 1e6},
                                     {"calls", qint64(scopes[i].calls)},
                                     {"work", qint64(scopes[i].work)}};
    const auto memoryAfter = diag::workingSetBytes();
    QJsonArray scheduler;
    {
      std::lock_guard lock(schedulerMutex_);
      for (auto row : schedulerSamples_) {
        const auto time = row.take("at_ns").toInteger();
        const auto begin = row.take("query_begin_ns").toInteger();
        if (time < phaseStart_) continue;
        row["at_ms"] = ms(time - phaseStart_);
        row["query_begin_at_ms"] = ms(begin - phaseStart_);
        row["query_duration_ms"] = ms(time - begin);
        scheduler.append(row);
      }
    }
    const auto cache = current()->renderView->theme().styleCacheStats();
    QJsonObject cacheStats{{"elements", cache.elements},
                           {"node_styles", cache.nodeStyles},
                           {"recipes", cache.recipes},
                           {"shared_recipes", cache.sharedRecipes},
                           {"engine_identities", cache.engine.identities},
                           {"resolved_styles", cache.engine.resolvedStyles},
                           {"cascades", cache.engine.cascades},
                           {"values", cache.engine.values},
                           {"shared_match_sets", cache.engine.matchSets},
                           {"resident_layouts", current()->renderView->residentLayoutCount()}};
    const auto text = themeTextPaintStats();
    QJsonObject textWork{{"calls", qint64(text.calls)},
                         {"lines", qint64(text.lines)},
                         {"format_ranges", qint64(text.formatRanges)},
                         {"selection_ranges", qint64(text.selectionRanges)}};
    QJsonObject result{{"phase", phases_[phase_].name},
                       {"active_throughout", !lostActivation_ && window_->isActiveWindow()},
                       {"exposed_throughout", !lostExposure_ && window_->windowHandle()->isExposed()},
                       {"windows_foreground_throughout", !lostForeground_ && hasWindowsForeground()},
                       {"foreground_loss", foregroundLoss_},
                       {"count", samples_.size()},
                       {"cadence_ms", phases_[phase_].intervalMs},
                       {"phase_wall_ms", ms(nowNs() - phaseStart_)},
                       {"start_steady_ns", phaseStart_},
                       {"sync", distribution(sync)},
                       {"scheduled_to_dispatch", distribution(queue)},
                       {"scheduled_to_paint", distribution(paint)},
                       {"dispatch_to_paint", distribution(dispatchPaint)},
                       {"producer_jitter", distribution(postJitter)},
                       {"paint_duration", distribution(paints_)},
                       {"heartbeat_delay", distribution(heartbeatDelays_)},
                       {"paint_count_by_tab", paintsByTab},
                       {"coalesced_input_count", coalesced},
                       {"inputs_without_paint", samples_.size() - paint.size()},
                       {"hidden_tab_paints", hiddenPaints_},
                       {"hidden_editor_events_tab_slash_qevent_type", hiddenEvents},
                       {"working_set_before", memoryBefore_},
                       {"working_set_after", memoryAfter},
                       {"process_after", diag::MemoryAccounting::process()},
                       {"resource_state_after", qEnvironmentVariableIsSet("MUFFIN_GUI_MEMORY_CENSUS") ? diag::MemoryAccounting::resources(*window_,documents_) : QJsonObject{}},
                       {"sampled_max_working_set", sampledMaxMemory_.load()},
                       {"scope_timings", scopeRows},
                       {"gui_thread_samples", scheduler},
                       {"draft_acknowledged_revision", qint64(current()->lastDraftSnapshotRevision)},
                       {"document_revision_before", qint64(phaseRevisionBefore_)},
                       {"document_revision", qint64(current()->session.document().revision())},
                       {"theme_stages", stages},
                       {"text_paint_work", textWork},
                       {"active_cache_stats", cacheStats},
                       {"samples", raw}};
    if (qEnvironmentVariableIsSet("MUFFIN_GUI_TRACE")) {
      result["timeline"] = trace_;
      result["timeline_dropped"] = traceDropped;
    }
    if (themeAcceptance_) result["scope_intervals"] = themeScopes_;
    check(hiddenPaints_ == 0, "a hidden editor painted during a measured phase");
    results_.append(result);
    std::fprintf(stderr, "%s: sync p95 %.1f ms, scheduled-paint p95 %.1f ms, memory %.1f MiB\n",
                 qPrintable(phases_[phase_].name), distribution(sync)["p95_ms"].toDouble(),
                 distribution(paint)["p95_ms"].toDouble(), memoryAfter / 1048576.0);
    QTimer::singleShot(1000, this, [this] { nextPhase(); });
  }
  void auditTheme(int index) {
    const QString targets[]{"gui-vlook", "github", "gui-phycat"};
    if (index == 6) {
      visualAuditDone_ = true;
      finish();
      return;
    }
    triggerTheme(targets[index % 3]);
    resetCaret();
    if (qEnvironmentVariableIsSet("MUFFIN_GUI_STATIC_VISUAL_AUDIT")) {
      // Only the untimed reference pass: normalize editor interaction state
      // without moving the system cursor or disabling theme animations.
      QEvent leave(QEvent::Leave);
      QCoreApplication::sendEvent(current()->renderView, &leave);
    }
    QTimer::singleShot(700, this, [this, index, target = targets[index % 3]] {
      auto* view = current()->renderView;
      const auto rectJson = [](QRectF r) { return QJsonArray{r.x(), r.y(), r.width(), r.height()}; };
      QJsonArray blocks;
      for (const auto& node : current()->session.document().root().children()) {
        const auto rect = view->nodeRect(node->id());
        if (rect.top() > view->viewport()->height()) break;
        const auto* block = view->blockLayoutForNode(node->id());
        if (!block) continue;
        QJsonArray cursors;
        if (const auto* text = block->inlineLayout()) {
          for (int offset : {0, 10, 20, 40}) {
            const auto caret = text->cursorRectForSourceOffset(offset);
            cursors.append(QJsonObject{
                {"source", offset}, {"rect", rectJson(caret)}, {"hit", text->hitTestSourceOffset(caret.center())}});
          }
        }
        blocks.append(QJsonObject{{"rect", rectJson(rect)}, {"cursors", cursors}});
      }
      const auto caret = view->effectiveCursorRect();
      check(caret.isValid() && view->hitForCursorPosition(view->cursorPosition()).isValid(),
            "invalid cursor geometry after theme switch");
      const auto hit = view->hitTest(view->mapDocumentToViewport(caret.center()));
      check(hit.isValid() && hit.blockId == view->cursorPosition().blockId && hit.textOffset == 5,
            "visible caret hit does not round-trip after theme switch");
      const auto screenshot = output_ + QString(".%1-%2.png").arg(target).arg(index / 3);
      check(window_->grab().save(screenshot), "theme screenshot save failed");
      if (index < 3) {
        const auto& doc = current()->session.document();
        ThemeManager reference;
        auto full = RenderTheme::fromDefinition(reference.definition(target));
        full.updateForViewport(view->viewport()->width(), view->viewport()->height());
        check(full.resolveDocumentCounters(doc) == view->theme().resolveDocumentCounters(doc),
              "theme counters differ from full reference");
        for (const auto& node : doc.root().children())
          check(full.generatedCounterFingerprint(node->id()) == view->theme().generatedCounterFingerprint(node->id()),
                "generated content differs from full reference");
      }
      visualAudit_.append(QJsonObject{{"target", target},
                                      {"cycle", index / 3},
                                      {"blocks", blocks},
                                      {"caret", rectJson(caret)},
                                      {"caret_hit_offset", hit.textOffset},
                                      {"font", view->theme().paragraphFont().toString()},
                                      {"text_color", view->theme().textColor().name(QColor::HexArgb)},
                                      {"screenshot", screenshot},
                                      {"full_counter_reference", index < 3},
                                      {"windows_foreground", hasWindowsForeground()},
                                      {"active", window_->isActiveWindow()},
                                      {"exposed", window_->windowHandle()->isExposed()}});
      if (index >= 3) {
        const auto previous = visualAudit_[index - 3].toObject();
        const auto actual = visualAudit_[index].toObject();
        for (const auto* field : {"blocks", "caret", "font", "text_color"})
          check(previous[field] == actual[field], "repeated GUI theme visual geometry/style differs");
      }
      auditTheme(index + 1);
    });
  }
  void finish() {
    if (!drainStart_) drainStart_ = nowNs();
    const auto sampleDrain = [this] {
      if (!qEnvironmentVariableIsSet("MUFFIN_GUI_MEMORY_CENSUS") || visualAuditDone_) return;
      draftMemorySamples_.append(QJsonObject{{"elapsed_ms", ms(nowNs() - drainStart_)},
                                             {"process", diag::MemoryAccounting::process()},
                                             {"resources", diag::MemoryAccounting::resources(*window_, documents_)}});
    };
    sampleDrain();
    bool drained = true;
    for (auto* tab : documents_) {
      if (tab->session.document().isModified() &&
          tab->lastDraftSnapshotRevision != tab->session.document().revision()) {
        drained = false;
        if (!drainRequested_) QMetaObject::invokeMethod(tab->draftTimer, "timeout", Qt::DirectConnection);
      }
    }
    drainRequested_ = true;
    sampleDrain();
    if (!drained && ms(nowNs() - drainStart_) < 60000) {
      QTimer::singleShot(50, this, [this] { finish(); });
      return;
    }
    QJsonArray draftRows;
    for (auto* tab : documents_)
      draftRows.append(QJsonObject{{"revision", qint64(tab->session.document().revision())},
                                   {"acknowledged", qint64(tab->lastDraftSnapshotRevision)}});
    const auto drainedWorkingSet = diag::workingSetBytes();
    const auto drainWaitMs = ms(nowNs() - drainStart_);
    if (qEnvironmentVariableIsSet("MUFFIN_GUI_MEMORY_CENSUS") && !memoryCensusDone_) {
      memoryCensusDone_ = true;
      QJsonArray rows;
      for (auto* tab : documents_) {
        diag::MemoryAccounting census;
        rows.append(QJsonObject{{"document", census.document(tab->session.document())},
                                {"layout", diag::MemoryAccounting::layout(*tab->renderView)}});
      }
      memoryCensus_ = QJsonObject{
          {"working_set_before_census", drainedWorkingSet},
          {"tabs", rows},
          {"resources", diag::MemoryAccounting::resources(*window_, documents_)},
          {"process_after_census", diag::MemoryAccounting::process()},
          {"working_set_after_census", diag::workingSetBytes()},
          {"method",
           "Untimed unique Qt buffer capacity and native object payload census; excludes allocator overhead, Qt "
           "shaping and OS font internals. Diagnostic temporary allocations can retain heap pages."}};
    }
    if (themeAcceptance_ && !foregroundWaitTimedOut_ && !visualAuditDone_) {
      memoryBeforeVisualAudit_ = drainedWorkingSet;
      drainWaitBeforeVisualAudit_ = drainWaitMs;
      auditTheme(0);
      return;
    }
    const auto screenshot = output_ + ".png";
    check(window_->grab().save(screenshot), "screenshot save failed");
    const auto* screen = window_->screen();
    const auto viewport = current()->renderView->viewport()->size();
    QFile css(source_);
    check(css.open(QIODevice::ReadOnly), "theme cannot be read");
    QJsonObject root{
        {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"MiB_per_tab", mib_},
        {"tabs", tabCount_},
        {"qpa", QGuiApplication::platformName()},
        {"qt_version", qVersion()},
        {"native_window_exposed", window_->windowHandle()->isExposed()},
        {"windows_foreground", hasWindowsForeground()},
        {"foreground_wait_required", qEnvironmentVariableIsSet("MUFFIN_GUI_REQUIRE_FOREGROUND")},
        {"foreground_wait_limit_ms", foregroundWaitLimitMs_},
        {"foreground_wait_timed_out", foregroundWaitTimedOut_},
        {"foreground_wait_ms", foregroundWaits_},
        {"open_foreground_wait_ms", openForegroundWaits_},
        {"foreground_wait_failure", foregroundWaitFailure_},
        {"foreground_before_activation", foregroundBeforeActivation_},
        {"foreground_identity", foregroundIdentity()},
        {"desktop", queueTrace ? queueTrace->desktopIdentity() : QJsonObject{}},
        {"active_window", window_->isActiveWindow()},
        {"pinned_window", qEnvironmentVariableIsSet("MUFFIN_GUI_PIN_WINDOW")},
        {"detailed_trace", qEnvironmentVariableIsSet("MUFFIN_GUI_TRACE")},
        {"input_phases_only", qEnvironmentVariableIsSet("MUFFIN_GUI_INPUT_ONLY")},
        {"typing_only", qEnvironmentVariableIsSet("MUFFIN_GUI_TYPING_ONLY")},
        {"theme_acceptance", themeAcceptance_},
        {"chain_index", chainIndex_},
        {"predecessor_pid", qint64(qEnvironmentVariable("MUFFIN_GUI_PREDECESSOR_PID").toULongLong())},
        {"startup_theme", themeAcceptance_ ? "night" : "gui-vlook"},
        {"visual_audit", visualAudit_},
        {"static_visual_audit", qEnvironmentVariableIsSet("MUFFIN_GUI_STATIC_VISUAL_AUDIT")},
        {"memory_census", memoryCensus_},
        {"draft_memory_samples", draftMemorySamples_},
        {"working_set_before_visual_audit", memoryBeforeVisualAudit_},
        {"working_set_after_visual_audit", drainedWorkingSet},
        {"draft_snapshots_enabled", !qEnvironmentVariableIsSet("MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT")},
        {"deep_css_profile", qEnvironmentVariableIsSet("MUFFIN_CSS_DEEP_PROFILE")},
        {"scheduler_sampling", qEnvironmentVariableIsSet("MUFFIN_GUI_SCHEDULER_SAMPLES")},
        {"input_delivery", nativeInput_ ? "windows_sendinput" : "qt_posted"},
        {"native_observations", nativeObservations_},
        {"native_posts", nativePosts_.size()},
        {"native_received", nativeReceived_},
        {"native_aborted", nativeAborted_.load()},
        {"native_foreground_mismatch", QString::number(nativeForegroundMismatch_)},
        {"native_expected_window", QString::number(nativeExpectedWindow_)},
        {"native_contaminated", nativeContaminated_},
        {"native_input_complete",
         !nativeInput_ || (!nativeAborted_ && !nativeContaminated_ && nativeReceived_ == count_)},
        {"profiling_mode", qEnvironmentVariable("MUFFIN_GUI_PROFILE", "minimal")},
        {"draft_drain",
         QJsonObject{{"complete", drained},
                     {"schema_version", 2},
                     {"wait_ms", visualAuditDone_ ? drainWaitBeforeVisualAudit_ : drainWaitMs},
                     {"working_set_after", visualAuditDone_ ? memoryBeforeVisualAudit_ : drainedWorkingSet},
                     {"tabs", draftRows}}},
        {"scheduler_state_sampling", qEnvironmentVariableIsSet("MUFFIN_GUI_SCHEDULER_SAMPLES")},
        {"queue_trace_version", 1},
        {"opaque_viewport", current()->renderView->viewport()->testAttribute(Qt::WA_OpaquePaintEvent)},
        {"clock_identity", queueTrace ? queueTrace->clockIdentity() : QJsonObject{}},
        {"steady_ns_at_identity", nowNs()},
        {"draft_snapshot_enabled", !qEnvironmentVariableIsSet("MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT")},
        {"draft_write_mode", "background_serial"},
        {"window_size", QString("%1x%2").arg(window_->width()).arg(window_->height())},
        {"viewport_size", QString("%1x%2").arg(viewport.width()).arg(viewport.height())},
        {"device_pixel_ratio", window_->devicePixelRatioF()},
        {"screen_refresh_hz", screen->refreshRate()},
        {"screen_logical_dpi", screen->logicalDotsPerInch()},
        {"font", current()->renderView->theme().paragraphFont().toString()},
        {"css_sha256",
         QString::fromLatin1(QCryptographicHash::hash(css.readAll(), QCryptographicHash::Sha256).toHex())},
        {"setup_ms", setupMs_},
        {"open", openRows_},
        {"screenshot", screenshot},
        {"phases", results_},
        {"method",
         "Windows QPA MainWindow and standard bindings; foreground/exposure checks determine validity. "
         "input_delivery distinguishes independent Qt-posted operations from foreground-guarded Win32 SendInput. "
         "SendInput is injected OS input, not physical keyboard/IME validation. Paint is completed QWidget dispatch, "
         "not DWM presentation. Nearest-rank quantiles and raw samples; 8ms heartbeat and 20ms working-set sampler. "
         "Isolated settings/files; autosave/update checks disabled; recovery-draft timers retained. Final drafts "
         "are requested and acknowledged asynchronously before the drained memory checkpoint."}};
    QFile file(output_);
    check(file.open(QIODevice::WriteOnly), "cannot write results");
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    if (qEnvironmentVariableIsSet("MUFFIN_GUI_REQUIRE_FOREGROUND"))
      writeForegroundReadiness(foregroundWaitTimedOut_ ? "foreground_timeout" : "result_written");
    std::fprintf(stderr, "GUI results: %s\n", qPrintable(output_));
    // QApplication's quit closes windows first; disposable edited buffers must
    // be clean before that close event, not only in the runner's destructor.
    for (auto* tab : documents_) tab->session.document().setModified(false);
    bool validChainRun = !foregroundWaitTimedOut_ && hasWindowsForeground() && drained && visualAuditDone_ &&
                         results_.size() == phases_.size();
    for (const auto value : results_) {
      const auto phase = value.toObject();
      validChainRun &= phase["active_throughout"].toBool() && phase["windows_foreground_throughout"].toBool() &&
                       phase["exposed_throughout"].toBool() && phase["inputs_without_paint"].toInt() == 0 &&
                       phase["hidden_tab_paints"].toInt() == 0;
      if (phase["phase"] == "theme_switch") validChainRun &= phase["coalesced_input_count"].toInt() == 0;
    }
    if (qEnvironmentVariableIsSet("MUFFIN_GUI_MEMORY_CENSUS")) {
      QJsonArray closed;
      const auto beforeClose = diag::workingSetBytes();
      for (auto* tab : documents_) {
        QPointer<DocumentTab> guard(tab);
        app_.viewports.remove(tab->renderView->viewport());
        for (auto it = app_.editorObjects.begin(); it != app_.editorObjects.end();) {
          if (it.value() == documents_.indexOf(tab))
            it = app_.editorObjects.erase(it);
          else
            ++it;
        }
        const int index = tabs_->indexOf(tab);
        QElapsedTimer timer;
        timer.start();
        tabs_->tabCloseRequested(index);
        check(guard.isNull(), "closed tab still owns document resources");
        closed.append(QJsonObject{{"close_handler_ms", timer.nsecsElapsed() / 1e6},
                                  {"working_set_after", diag::workingSetBytes()},
                                  {"process", diag::MemoryAccounting::process()},
                                  {"destroyed", true}});
      }
      documents_.clear();
      app_.activeTab = {};
      QTimer::singleShot(1000, this, [this, validChainRun, beforeClose, closed] {
        const auto afterOneSecond = diag::workingSetBytes();
        QTimer::singleShot(4000, this, [this, validChainRun, beforeClose, closed, afterOneSecond] {
          QFile file(output_);
          check(file.open(QIODevice::ReadOnly), "read closure checkpoint");
          auto root = QJsonDocument::fromJson(file.readAll()).object();
          file.close();
          root["memory_close"] =
              QJsonObject{{"working_set_before", beforeClose},
                          {"closed", closed},
                          {"process_after_5s", diag::MemoryAccounting::process()},
                          {"resource_state_after_5s", diag::MemoryAccounting::resources(*window_, documents_)},
                          {"working_set_after_1s", afterOneSecond},
                          {"working_set_after_5s", diag::workingSetBytes()},
                          {"remaining_tabs", tabs_->count()},
                          {"all_original_tabs_destroyed", true},
                          {"method",
                           "Standard close bindings after drafts drained and untimed visual reference. No "
                           "heap/working-set trim. One empty tab remains."}};
          check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write closure checkpoint");
          file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
          file.close();
          handOffChain(validChainRun);
          app_.quit();
        });
      });
    } else {
      handOffChain(validChainRun);
      app_.quit();
    }
  }
  ProbeApplication& app_;
  QString output_;
  double mib_;
  int tabCount_, count_;
  QString source_;
  QString chainFile_;
  int chainIndex_ = -1;
  QJsonObject chainPlan_;
#ifdef Q_OS_WIN
  HANDLE predecessorHandle_ = nullptr;
#endif
  QTemporaryDir files_;
  std::unique_ptr<MainWindow> window_;
  QTabWidget* tabs_ = nullptr;
  QVector<DocumentTab*> documents_;
  QVector<Phase> phases_;
  int phase_ = -1, hiddenPaints_ = 0;
  QVector<Sample> samples_;
  QVector<double> paints_, heartbeatDelays_, hiddenHeights_;
  QVector<int> paintsByTab_;
  QVector<quint64> hiddenRevisions_;
  QVector<std::shared_ptr<const CssThemeSheet>> hiddenSheets_;
  QHash<QString, QVector<double>> probes_;
  QHash<QString, QPair<int, double>> hiddenEvents_;
  QHash<int, qint64> duringOperationPaint_;
  QHash<int, qint64> duringOperationPaintStart_;
  std::mutex schedulerMutex_;
  QVector<QJsonObject> schedulerSamples_;
  QJsonArray results_, openRows_, trace_;
  qint64 openCallReturned_ = 0, openInstallFinished_ = 0, openFirstPaint_ = 0;
  bool openPaintForeground_ = false;
  QTimer heartbeat_;
  qint64 lastBeat_ = 0, memoryBefore_ = 0, phaseStart_ = 0, setupStart_ = 0;
  double setupMs_ = 0;
  bool recording_ = false, producerDone_ = false, lostActivation_ = false, lostExposure_ = false,
       lostForeground_ = false;
  QJsonObject foregroundLoss_;
  bool inOperation_ = false;
  const bool themeAcceptance_ = qEnvironmentVariableIsSet("MUFFIN_GUI_THEME_ACCEPTANCE");
  bool visualAuditDone_ = false;
  qint64 memoryBeforeVisualAudit_ = 0;
  bool memoryCensusDone_ = false;
  QJsonObject memoryCensus_;
  QJsonArray draftMemorySamples_;
  double drainWaitBeforeVisualAudit_ = 0;
  QJsonArray visualAudit_, themeScopes_;
  const bool nativeInput_ = qEnvironmentVariableIsSet("MUFFIN_GUI_NATIVE_INPUT");
  std::atomic<bool> nativeAborted_{false};
  quintptr nativeForegroundMismatch_ = 0, nativeExpectedWindow_ = 0;
  bool nativeContaminated_ = false;
  QJsonArray nativeObservations_;
  int nativeReceived_ = 0, nativeCurrent_ = -1;
  std::mutex nativeMutex_;
  QVector<QPair<qint64, qint64>> nativePosts_;
  DocumentTab* beforeTab_ = nullptr;
  SelectionRange beforeSelection_;
  quint64 beforeRevision_ = 0;
  quint64 phaseRevisionBefore_ = 0;
  int beforeScroll_ = 0;
  qint64 drainStart_ = 0;
  qint64 foregroundWaitStart_ = 0, foregroundStableStart_ = 0;
  qint64 openForegroundWaitStart_ = 0, openForegroundStableStart_ = 0;
  QJsonArray openForegroundWaits_;
  bool foregroundWaitAnnounced_ = false, foregroundWaitTimedOut_ = false;
  const int foregroundWaitLimitMs_ = [] {
    if (!qEnvironmentVariableIsSet("MUFFIN_GUI_FOREGROUND_WAIT_MS")) return 120000;
    bool ok = false;
    const auto value = qEnvironmentVariableIntValue("MUFFIN_GUI_FOREGROUND_WAIT_MS", &ok);
    check(ok && value >= 1000 && value <= 3600000, "foreground wait must be 1000..3600000 ms");
    return value;
  }();
  QJsonArray foregroundWaits_;
  QJsonObject foregroundWaitFailure_;
  QJsonObject foregroundBeforeActivation_;
  bool drainRequested_ = false;
  std::atomic<bool> recordingMemory_{false};
  std::atomic<qint64> sampledMaxMemory_{0};
  std::jthread producer_, sampler_;
};
}  // namespace

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "windows");
  ProbeApplication app(argc, argv);
  GuiQueueTrace queue;
  queueTrace = &queue;
  app.installNativeEventFilter(&queue);
  check(QGuiApplication::platformName() == "windows", "use the Windows QPA, never offscreen");
  check(argc >= 4, "usage: MuffinGuiPerformanceProbe output.json MiB tabs [samples=80]");
  const auto output = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
  const auto mib = QString::fromLocal8Bit(argv[2]).toDouble();
  const auto tabs = QString::fromLocal8Bit(argv[3]).toInt();
  const auto samples = argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt() : 80;
  check(mib >= .01 && mib <= 30 && tabs >= 1 && tabs <= 3 && samples >= 4 && samples <= 500, "invalid dimensions");
  QApplication::setOrganizationName("MuffinBenchmarks");
  const auto name = "GuiPerformanceProbe-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  QApplication::setApplicationName(name);
  QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  QSettings s;
  s.setValue("update/autoCheck", false);
  s.setValue("files/autoSave", false);
  s.setValue("files/autoSaveOnSwitch", false);
  s.setValue("view/sourceMode", false);
  const bool themeAcceptance = qEnvironmentVariableIsSet("MUFFIN_GUI_THEME_ACCEPTANCE");
  s.setValue("appearance/themeName", themeAcceptance ? "night" : "gui-vlook");
  s.setValue("appearance/fontSizePx", 16);
  s.setValue("appearance/zoomPercent", 100);
  // Keep actual theme animations; disable persistence work only for disposable buffers.
  s.setValue("appearance/reducedMotion", false);
  s.sync();
  LanguageManager::instance().setLanguage("en");
  const auto source = QStringLiteral(MUFFIN_SOURCE_DIR "/tests/fixtures/theme/vlook-fancy-dark/vlook-fancy-dark.css");
  const auto dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  const auto themes = ThemeManager::themesDirectory();
  check(QDir().mkpath(themes) && ThemeManager::installCssTheme(source, themes), "cannot stage fixed theme");
  check(QFile::rename(themes + "/vlook-fancy-dark.css", themes + "/gui-vlook.css"), "theme alias staging failed");
  if (themeAcceptance) {
    const auto phycat = qEnvironmentVariable("MUFFIN_GUI_PHYCAT_SOURCE");
    check(!phycat.isEmpty() && ThemeManager::installCssTheme(phycat, themes), "cannot stage fixed Phycat theme");
    check(QFile::rename(themes + '/' + QFileInfo(phycat).fileName().toLower(), themes + "/gui-phycat.css"),
          "Phycat alias staging failed");
  }
  configurePerformanceLogging(false);
  qInstallMessageHandler(messageHandler);
  enableThemePerformance(qEnvironmentVariable("MUFFIN_GUI_PROFILE") != "off");
  int exitCode;
  {
    Runner runner(app, output, mib, tabs, samples, source);
    QTimer::singleShot(0, &runner, [&runner] { runner.start(); });
    exitCode = app.exec();
  }
  // Only the unique app-owned directory created by this invocation is removed.
  check(QFileInfo(dataRoot).fileName() == name && QDir(dataRoot).absolutePath().contains("MuffinBenchmarks"),
        "isolated data root validation failed");
  QDir(dataRoot).removeRecursively();
  return exitCode;
}

#pragma once

#include <QAbstractNativeEventFilter>
#include <QJsonObject>
#include <functional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

#include <vector>
#endif

// Benchmark-only observations. CPU time is coarse Windows thread accounting;
// cycles have finer resolution but cannot be converted to milliseconds at a
// fixed clock rate. Neither identifies scheduler ready time or a blocking cause.
class GuiQueueTrace final : public QAbstractNativeEventFilter {
 public:
  GuiQueueTrace() {
#ifdef Q_OS_WIN
    thread_ = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
#endif
  }
  ~GuiQueueTrace() override {
#ifdef Q_OS_WIN
    if (thread_) CloseHandle(thread_);
#endif
  }
  QJsonObject threadSample() const {
    QJsonObject result;
#ifdef Q_OS_WIN
    FILETIME created{}, exited{}, kernel{}, user{};
    ULONG64 cycles = 0;
    if (thread_ && GetThreadTimes(thread_, &created, &exited, &kernel, &user)) {
      const auto ticks = [](FILETIME time) { return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
      result["cpu_ms"] = (ticks(kernel) + ticks(user)) / 10000.0;
    }
    if (thread_ && QueryThreadCycleTime(thread_, &cycles)) result["cycles"] = qint64(cycles);
#endif
    return result;
  }
  QJsonObject clockIdentity() const {
    QJsonObject result;
#ifdef Q_OS_WIN
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    result = {{"process_id", qint64(GetCurrentProcessId())},
              {"gui_thread_id", qint64(GetCurrentThreadId())},
              {"qpc_ticks", qint64(counter.QuadPart)},
              {"qpc_frequency", qint64(frequency.QuadPart)},
              {"tick_ms", qint64(GetTickCount64())}};
#endif
    return result;
  }
  QJsonObject desktopIdentity() const {
    QJsonObject result;
#ifdef Q_OS_WIN
    const auto objectName = [](HANDLE handle) {
      WCHAR name[256]{};
      DWORD needed = 0;
      return GetUserObjectInformationW(handle, UOI_NAME, name, sizeof(name), &needed) ? QString::fromWCharArray(name)
                                                                                      : QString{};
    };
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    result["process_session"] = qint64(session);
    result["window_station"] = objectName(GetProcessWindowStation());
    result["thread_desktop"] = objectName(GetThreadDesktop(GetCurrentThreadId()));
    const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    result["input_desktop_open"] = bool(input);
    if (input) {
      result["input_desktop"] = objectName(input);
      CloseDesktop(input);
    } else
      result["input_desktop_error"] = qint64(GetLastError());
#endif
    return result;
  }
  // Sample on the background collector only. This does not suspend the GUI or
  // require a kernel profiling session. Only our GUI thread's fields escape.
  QJsonObject schedulingSample() const {
    auto result = threadSample();
#ifdef Q_OS_WIN
    using Query = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    static const auto query =
        reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return result;
    thread_local std::vector<unsigned char> buffer(1024 * 1024);
    ULONG needed = 0;
    auto status = query(SystemProcessInformation, buffer.data(), ULONG(buffer.size()), &needed);
    if (status == NTSTATUS(0xc0000004L) && needed < 64 * 1024 * 1024) {
      buffer.resize(needed + 65536);
      status = query(SystemProcessInformation, buffer.data(), ULONG(buffer.size()), &needed);
    }
    if (status < 0) return result;
    for (size_t offset = 0; offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= buffer.size();) {
      const auto* process = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer.data() + offset);
      if (quintptr(process->UniqueProcessId) == GetCurrentProcessId()) {
        const auto* threads = reinterpret_cast<const SYSTEM_THREAD_INFORMATION*>(process + 1);
        if (offset + sizeof(*process) + size_t(process->NumberOfThreads) * sizeof(*threads) > buffer.size()) break;
        for (ULONG i = 0; i < process->NumberOfThreads; ++i)
          if (quintptr(threads[i].ClientId.UniqueThread) == threadId_) {
            result["thread_state"] = int(threads[i].ThreadState);
            result["wait_reason"] = int(threads[i].WaitReason);
            result["priority"] = int(threads[i].Priority);
            break;
          }
        break;
      }
      if (!process->NextEntryOffset) break;
      offset += process->NextEntryOffset;
    }
    // Querying the system process table can itself take time. CPU/cycles are
    // read afterwards; the caller retains the whole sampling interval too.
    const auto cpu = threadSample();
    for (auto it = cpu.begin(); it != cpu.end(); ++it) result[it.key()] = it.value();
#endif
    return result;
  }
  std::function<void(QJsonObject)> nativeMessage;
  bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override {
#ifdef Q_OS_WIN
    if (nativeMessage) {
      const auto& msg = *static_cast<MSG*>(message);
      nativeMessage({{"message", qint64(msg.message)},
                     {"hwnd", QString::number(quintptr(msg.hwnd))},
                     {"message_tick_ms", qint64(msg.time)}});
    }
#endif
    return false;
  }

 private:
#ifdef Q_OS_WIN
  HANDLE thread_ = nullptr;
  DWORD threadId_ = GetCurrentThreadId();
#endif
};

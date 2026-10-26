#pragma once

#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>
#include <QTimer>

namespace muffin {

namespace file_open_preferences {
inline constexpr char kOpenInTabs[] = "files/openInTabs";
bool openInTabs();
} // namespace file_open_preferences

class MainWindow;

// Shared by OS FileOpen events and requests forwarded by another process.
// All paths enter MainWindow::openFile, preserving tab ownership, unsaved work,
// duplicate detection, and file-open protection.
class FileOpenDispatcher final : public QObject {
public:
  explicit FileOpenDispatcher(QObject* parent = nullptr);
  bool setInitialWindow(MainWindow* window);
  void openFiles(const QStringList& paths);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  struct Request {
    QStringList paths;
    bool newWindow = false;
  };
  void dispatch();
  MainWindow* targetWindow() const;

  bool ready_ = false;
  QPointer<MainWindow> initialWindow_;
  QPointer<MainWindow> lastActiveWindow_;
  QQueue<Request> pending_;
  QTimer retry_;
};

} // namespace muffin

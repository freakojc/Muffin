#pragma once

#include <QAbstractFileIconProvider>
#include <QApplication>
#include <QEvent>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QWidget>

// macOS file icons use native workspace services, which are unavailable to
// headless test processes. These fixtures exercise row/document interaction,
// so use an icon provider without native service calls for their real models.
class FileTreeTestIcons final : public QObject {
public:
  explicit FileTreeTestIcons(QApplication& app) {
#ifdef Q_OS_MACOS
    app.installEventFilter(this);
#else
    Q_UNUSED(app);
#endif
  }

  void apply(QWidget& widget) {
#ifdef Q_OS_MACOS
    for (auto* model : widget.findChildren<QFileSystemModel*>()) {
      model->setIconProvider(&provider_);
    }
#else
    Q_UNUSED(widget);
#endif
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::Show) {
      if (auto* dialog = qobject_cast<QFileDialog*>(watched)) {
        dialog->setIconProvider(&provider_);
      }
    }
    return false;
  }

private:
  class Provider final : public QAbstractFileIconProvider {
  public:
    QIcon icon(IconType) const override { return {}; }
    QIcon icon(const QFileInfo&) const override { return {}; }
  } provider_;
};

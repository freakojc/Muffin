#pragma once

#include "app/FileOpenProtection.h"

#include <QWidget>

class QDialogButtonBox;
class QLabel;

namespace muffin {

class RenderTheme;

class FileOpenConfirmation final : public QWidget {
  Q_OBJECT
public:
  explicit FileOpenConfirmation(QWidget* parent = nullptr);
  void setFile(const QString& path, file_open_protection::Warnings warnings);
  void setTheme(const RenderTheme& theme);
  void retranslateUi();

signals:
  void confirmed();
  void canceled();

private:
  QLabel* title_ = nullptr;
  QLabel* file_ = nullptr;
  QLabel* extensionWarning_ = nullptr;
  QLabel* sizeWarning_ = nullptr;
  QDialogButtonBox* buttons_ = nullptr;
};

}  // namespace muffin

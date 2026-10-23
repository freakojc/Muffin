#pragma once

// Keyboard-driven file picker with asynchronous directory scanning and
// relative path matching, independent of file-tree expansion.
//
// NOTE: the .cpp contains tr() calls and follows the lupdate rule in CLAUDE.md
// (fully-qualified muffin::QuickOpenDialog::method() defs + anonymous-namespace
// helpers, no namespace muffin {} wrapper) so translations land under the
// muffin::QuickOpenDialog context.

#include <QDialog>
#include <QString>
#include <QStringList>
#include "app/WorkspaceFiles.h"

class QLineEdit;
class QListWidget;
class QLabel;
class QTimer;

namespace muffin {
struct ThemeDefinition;

class MUFFIN_UI_EXPORT QuickOpenDialog : public QDialog {
  Q_OBJECT

public:
  explicit QuickOpenDialog(QWidget* parent = nullptr);
  ~QuickOpenDialog() override;

  void setCandidates(QStringList paths);
  void setWorkspace(const QString& root, const QStringList& extensions);
  void setThemeDefinition(const ThemeDefinition& definition);
  QString selectedPath() const;

protected:
  // Routes Enter/Esc/Up/Down typed in the filter box to accept/reject/navigate
  // the list, so the picker is fully keyboard-driven without losing typing focus.
  bool eventFilter(QObject* watched, QEvent* event) override;
  void changeEvent(QEvent* event) override;

private:
  void applyFilter(const QString& text);
  void addPathItem(const QString& path);
  void retranslateUi();

  QLineEdit* filterEdit_ = nullptr;
  QListWidget* list_ = nullptr;
  QLabel* scopeLabel_ = nullptr;
  QLabel* statusLabel_ = nullptr;
  QTimer* filterTimer_ = nullptr;
  QStringList candidates_;
  QString root_;
  QString lastFilter_;
  SearchCancellation cancel_;
  bool scanning_ = false;
  bool truncated_ = false;
  int unreadable_ = 0;
};

} // namespace muffin

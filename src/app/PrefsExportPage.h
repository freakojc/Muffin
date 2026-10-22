#pragma once
#include "app/PreferencesPage.h"
class QLabel;
class QLineEdit;
class QListWidget;
class QScrollArea;
namespace muffin {
class PrefsExportPage final : public PreferencesPage {
  Q_OBJECT
public:
  explicit PrefsExportPage(QWidget* parent = nullptr);
  void retranslateUi() override;
private:
  void rebuildPage();
  void refreshPandocStatus();
  QListWidget* formatList_ = nullptr;
  QLabel* sectionLabel_ = nullptr;
  QScrollArea* content_ = nullptr;
  QLineEdit* pandocPathEdit_ = nullptr;
  QLabel* pandocStatus_ = nullptr;
};
}  // namespace muffin

#pragma once

#include "app/PreferencesPage.h"
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QDoubleSpinBox;

namespace muffin {

class PrefsFilesPage final : public PreferencesPage {
  Q_OBJECT

public:
  explicit PrefsFilesPage(QWidget* parent = nullptr);

  void retranslateUi() override;

signals:
  void clearRecentFilesRequested();
  void outlineFoldableChanged(bool foldable);
  void restoreDraftsRequested();

private:
  void buildUi();
  void loadSettings();
  void addTextExtensions();
  void refreshTextExtensionChips();
  void saveTextExtensions();
  void updateSizeLimitControls();

  QLabel* startupLabel_ = nullptr;
  QComboBox* startupCombo_ = nullptr;
  QLabel* outlineLabel_ = nullptr;
  QCheckBox* outlineFoldableCheck_ = nullptr;
  QLabel* defaultExtLabel_ = nullptr;
  QComboBox* defaultExtCombo_ = nullptr;
  QLabel* openProtectionLabel_ = nullptr;
  QCheckBox* extensionProtectionCheck_ = nullptr;
  QWidget* extensionOptions_ = nullptr;
  QLabel* textExtensionsLabel_ = nullptr;
  QLineEdit* textExtensionsEdit_ = nullptr;
  QLabel* textExtensionsHint_ = nullptr;
  QWidget* textExtensionChips_ = nullptr;
  QPushButton* addExtensionButton_ = nullptr;
  QPushButton* resetExtensionsButton_ = nullptr;
  QStringList textExtensions_;
  QCheckBox* sizeProtectionCheck_ = nullptr;
  QLabel* sizeProtectionHint_ = nullptr;
  QLabel* sizeLimitLabel_ = nullptr;
  QDoubleSpinBox* sizeLimitSpin_ = nullptr;
  QComboBox* sizeLimitUnit_ = nullptr;
  qint64 sizeLimitBytes_ = 0;
  qint64 sizeLimitUnitBytes_ = 1024 * 1024;
  QLabel* saveLabel_ = nullptr;
  QCheckBox* autoSaveCheck_ = nullptr;
  QCheckBox* autoSaveSwitchCheck_ = nullptr;
  QPushButton* restoreDraftButton_ = nullptr;
  QLabel* recentLabel_ = nullptr;
  QCheckBox* recordHistoryCheck_ = nullptr;
  QPushButton* clearHistoryButton_ = nullptr;
  QLabel* dropLabel_ = nullptr;
  QLabel* dropFolderLabel_ = nullptr;
  QComboBox* dropFolderCombo_ = nullptr;
  QLabel* dropMdLabel_ = nullptr;
  QComboBox* dropMdCombo_ = nullptr;
  QLabel* dropImportLabel_ = nullptr;
  QComboBox* dropImportCombo_ = nullptr;
  QLabel* winIntegrationLabel_ = nullptr;
  QCheckBox* winContextMenuCheck_ = nullptr;
  QCheckBox* winAssociateCheck_ = nullptr;
  QPushButton* winSetDefaultButton_ = nullptr;
};

} // namespace muffin

#include "app/FileOpenConfirmation.h"

#include "theme/RenderTheme.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

muffin::FileOpenConfirmation::FileOpenConfirmation(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("FileOpenConfirmation"));
  setAutoFillBackground(true);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(28, 28, 28, 28);
  auto* content = new QWidget(this);
  content->setMaximumWidth(620);
  auto* column = new QVBoxLayout(content);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(20);
  title_ = new QLabel(content);
  title_->setObjectName(QStringLiteral("openConfirmationTitle"));
  file_ = new QLabel(content);
  file_->setObjectName(QStringLiteral("openConfirmationFile"));
  extensionWarning_ = new QLabel(content);
  extensionWarning_->setObjectName(QStringLiteral("extensionWarning"));
  sizeWarning_ = new QLabel(content);
  sizeWarning_->setObjectName(QStringLiteral("sizeWarning"));
  for (auto* label : {title_, file_, extensionWarning_, sizeWarning_}) {
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    column->addWidget(label);
  }
  buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, content);
  buttons_->setCenterButtons(true);
  buttons_->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("confirmFileOpen"));
  buttons_->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("cancelFileOpen"));
  column->addWidget(buttons_);
  layout->addStretch();
  auto* row = new QHBoxLayout();
  row->addStretch(1);
  row->addWidget(content, 100);
  row->addStretch(1);
  layout->addLayout(row);
  layout->addStretch();
  setFocusProxy(buttons_->button(QDialogButtonBox::Cancel));
  connect(buttons_, &QDialogButtonBox::accepted, this, &FileOpenConfirmation::confirmed);
  connect(buttons_, &QDialogButtonBox::rejected, this, &FileOpenConfirmation::canceled);
  retranslateUi();
}

void muffin::FileOpenConfirmation::setFile(const QString& path, file_open_protection::Warnings warnings) {
  const QFileInfo info(path);
  file_->setText(QStringLiteral("%1\n%2").arg(
      info.fileName(), QLocale().formattedDataSize(info.size(), 2, QLocale::DataSizeTraditionalFormat)));
  file_->setToolTip(QDir::toNativeSeparators(info.absoluteFilePath()));
  extensionWarning_->setVisible(warnings.extension);
  sizeWarning_->setVisible(warnings.size);
}

void muffin::FileOpenConfirmation::setTheme(const RenderTheme& theme) {
  QPalette colors = palette();
  colors.setColor(QPalette::Window, theme.viewportBackgroundColor());
  colors.setColor(QPalette::WindowText, theme.textColor());
  colors.setColor(QPalette::Button, theme.pageBackgroundColor());
  colors.setColor(QPalette::ButtonText, theme.textColor());
  setPalette(colors);
  title_->setFont(theme.headingFont(2));
  setStyleSheet(QStringLiteral(
                    "QWidget#FileOpenConfirmation { background:%5; }"
                    "QLabel { color:%1; background:transparent; }"
                    "QLabel#openConfirmationFile { color:%2; }"
                    "QPushButton { color:%1; background:%3; border:1px solid %2; border-radius:6px; padding:7px 24px; }"
                    "QPushButton:hover, QPushButton:focus { border-color:%1; background:%4; }"
                    "QPushButton:disabled { color:%2; }")
                    .arg(theme.textColor().name(), theme.mutedTextColor().name(), theme.pageBackgroundColor().name(),
                         theme.selectionColor().name(), theme.viewportBackgroundColor().name()));
}

void muffin::FileOpenConfirmation::retranslateUi() {
  title_->setText(tr("Open this file?"));
  extensionWarning_->setText(tr("This file is not a Markdown or text file. Are you sure you want to open it?"));
  sizeWarning_->setText(
      tr("Opening this large file may take a long time and use a lot of memory. Are you sure you want to open it?"));
}

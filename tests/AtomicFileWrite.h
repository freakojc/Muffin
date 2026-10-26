#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QString>
#include <QThread>

namespace muffin::test {

// Model an external atomic writer independently of notification/UI assertions.
// Windows can briefly deny replacement while another process inspects a file.
// Retry only a failed rename, with a fresh temporary file and a fixed deadline;
// never fall back to truncating the target or pump application events here.
inline bool writeFileAtomically(const QString& path, const QByteArray& data, QString* error = nullptr,
                                int renameTimeoutMs = 1000) {
  QElapsedTimer elapsed;
  elapsed.start();
  if (error)
    error->clear();
  while (true) {
    bool retryRename = false;
    {
      QSaveFile file(path);
      file.setDirectWriteFallback(false);
      if (!file.open(QIODevice::WriteOnly)) {
        if (error)
          *error = QStringLiteral("open: %1").arg(file.errorString());
        return false;
      }
      if (file.write(data) != data.size()) {
        if (error)
          *error = QStringLiteral("write: %1").arg(file.errorString());
        return false;
      }
      if (file.commit()) {
        if (error)
          error->clear();
        return true;
      }
      if (error)
        *error = QStringLiteral("commit: %1").arg(file.errorString());
#ifdef Q_OS_WIN
      retryRename = file.error() == QFileDevice::RenameError;
#endif
    } // Discard the failed temporary file before another attempt.
    const qint64 remaining = renameTimeoutMs - elapsed.elapsed();
    if (!retryRename || remaining <= 0)
      return false;
    QThread::msleep(static_cast<unsigned long>(qMin<qint64>(10, remaining)));
  }
}

} // namespace muffin::test

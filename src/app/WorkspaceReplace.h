#pragma once
#include "app/WorkspaceSearch.h"

namespace muffin {
struct WorkspaceChange {
  QString path, beforeText, afterText;
  QByteArray before, after;
  int matches = 0;
};
struct WorkspaceReplaceReport {
  QString backupDirectory;
  QStringList changed, skipped;
  QString error;
};
class MUFFIN_UI_EXPORT WorkspaceReplace final {
public:
  static QVector<WorkspaceChange> prepare(const WorkspaceSearchResult& result,
                                         const QString& replacement, QStringList* skipped);
  static WorkspaceReplaceReport apply(const QString& root, const QVector<WorkspaceChange>& changes,
      const QString& recoveryRoot, const SearchCancellation& cancel);
  static QString latestRecovery(const QString& recoveryRoot);
  static WorkspaceReplaceReport restore(const QString& directory, const OpenSearchBuffers& buffers,
                                        const SearchCancellation& cancel);
};
}  // namespace muffin

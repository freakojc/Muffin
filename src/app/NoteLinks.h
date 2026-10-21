#pragma once
#include "app/WorkspaceReplace.h"
#include "document/OutlineBuilder.h"

namespace muffin {
struct NoteLinkChange {
  WorkspaceChange file;
  bool unsaved = false;
};
struct NoteLinkPlan {
  QString root;
  QVector<NoteLinkChange> changes;
  QStringList skipped;
  bool complete = true;
};
class MUFFIN_UI_EXPORT NoteLinks final {
public:
  static bool isMarkdownPath(const QString& path);
  static NodeId headingTarget(const QVector<OutlineEntry>& outline, const QString& fragment);
  // Only AST-confirmed Markdown destinations are edited; code and plain text stay literal.
  static QString rewrite(const QString& text, const QString& sourcePath,
                         const QString& oldPath, const QString& newPath, int* count = nullptr);
  static NoteLinkPlan prepare(const QString& root, const QString& oldPath, const QString& newPath,
                             const OpenSearchBuffers& buffers, const SearchCancellation& cancel);
};
}

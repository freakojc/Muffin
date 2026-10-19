#pragma once
#include "app/WorkspaceFiles.h"
#include "editor/DocumentSearch.h"
#include "io/TextFileFormat.h"
#include <QHash>
#include <functional>

namespace muffin {
struct OpenSearchBuffer { QString text; bool modified = false; bool busy = false; };
using OpenSearchBuffers = QHash<QString, OpenSearchBuffer>;
struct WorkspaceQuery {
  QString root, pattern, includePath, excludePath;
  QStringList extensions;
  SearchOptions options;
  qint64 maxFileBytes = 8 * 1024 * 1024;
  int maxMatches = 10000;
};
struct WorkspaceHit {
  SearchMatch match;
  int line = 1;
  QString context;
  int highlight = 0;
};
struct WorkspaceSearchFile {
  QString path, text;
  QByteArray original;
  TextFileFormat format;
  QVector<WorkspaceHit> hits;
  bool modified = false;
  bool replaceable = false;
};
struct WorkspaceSearchResult {
  WorkspaceQuery query;
  QVector<WorkspaceSearchFile> files;
  QStringList skipped;
  QString error;
  bool complete = true;
  int matches = 0;
};
class MUFFIN_UI_EXPORT WorkspaceSearch final {
public:
  static WorkspaceSearchResult run(const WorkspaceQuery& query, const OpenSearchBuffers& buffers,
      const SearchCancellation& cancel,
      const std::function<void(const WorkspaceSearchFile&)>& progress = {});
};
}  // namespace muffin

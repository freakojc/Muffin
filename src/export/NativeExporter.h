#pragma once

#include "export/ExportOptions.h"
#include "theme/RenderTheme.h"

namespace muffin {
class MarkdownDocument;
namespace mermaid::editor {
class MermaidRenderCache;
}

// Uses the native block renderer with a fixed 96 CSS px/inch, independent of
// editor zoom and monitor DPI. Both functions write only the supplied path.
bool exportNativePdf(const QString& path, const MarkdownDocument& document, RenderTheme theme,
                     const QString& sourcePath, const PdfExportOptions& options,
                     mermaid::editor::MermaidRenderCache* cache = nullptr);
// A single PNG is bounded to 64 million pixels; callers explain failure to users.
bool exportNativeImage(const QString& path, const MarkdownDocument& document, RenderTheme theme,
                       const QString& sourcePath, const ImageExportOptions& options,
                       mermaid::editor::MermaidRenderCache* cache = nullptr);
} // namespace muffin

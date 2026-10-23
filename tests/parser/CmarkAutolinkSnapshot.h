#pragma once

#include "CmarkFootnoteSnapshot.h"
extern "C" {
#include "parser.h"
#include "syntax_extension.h"
}

namespace autolink_test {
// An earlier postprocessor can create adjacent text after core consolidation.
inline cmark_node* inject(cmark_syntax_extension*, cmark_parser*, cmark_node* root) {
  auto* paragraph = cmark_node_new(CMARK_NODE_PARAGRAPH);
  for (const auto* value : {"injected", "@", "example.com"}) {
    auto* text = cmark_node_new(CMARK_NODE_TEXT);
    cmark_node_set_literal(text, value);
    cmark_node_append_child(paragraph, text);
  }
  cmark_node_append_child(root, paragraph);
  return root;
}
inline QJsonArray run(const QJsonArray& inputs) {
  QJsonArray records;
  cmark_gfm_core_extensions_ensure_registered();
  auto* custom = cmark_syntax_extension_new("autolink-regression-injector");
  cmark_syntax_extension_set_postprocess_func(custom, inject);
  for (const auto* mode : {"production", "preceding-postprocessor", "duplicate-autolink", "no-autolink"}) {
    auto* parser = cmark_parser_new(CMARK_OPT_FOOTNOTES);
    if (QByteArray(mode) == "preceding-postprocessor")
      cmark_parser_attach_syntax_extension(parser, custom);
    for (const auto* name : {"table", "autolink", "tasklist", "math"}) {
      if (QByteArray(mode) == "no-autolink" && QByteArray(name) == "autolink")
        continue;
      cmark_parser_attach_syntax_extension(parser, cmark_find_syntax_extension(name));
    }
    if (QByteArray(mode) == "duplicate-autolink")
      cmark_parser_attach_syntax_extension(parser, cmark_find_syntax_extension("autolink"));
    for (const bool chunks : {false, true})
      for (const auto& entry : inputs) {
        const auto input = entry.toObject();
        const auto bytes = input["source"].toString().toUtf8();
        if (qEnvironmentVariableIsSet("MUFFIN_CMARK_ORACLE_TRACE"))
          fprintf(stderr, "Autolink oracle %s mode=%s chunks=%d\n", qPrintable(input["name"].toString()), mode, chunks);
        records.append(
            QJsonObject{{"name", input["name"]},
                        {"mode", mode},
                        {"byte_chunks", chunks},
                        {"result", footnote_test::snapshot(parser, bytes, chunks ? 1 : qMax(1, int(bytes.size())),
                                                           QByteArray(mode) != "no-autolink")}});
      }
    cmark_parser_free(parser);
  }
  cmark_syntax_extension_free(cmark_get_default_mem_allocator(), custom);
  return records;
}
} // namespace autolink_test

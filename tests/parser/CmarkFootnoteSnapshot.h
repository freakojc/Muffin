#pragma once

// Shared by the frozen, pre-change oracle and the permanent regression test.
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <cstdlib>

extern "C" {
#include "cmark-gfm-core-extensions.h"
#include "cmark-gfm-extension_api.h"
#include "node.h"
}

namespace footnote_test {
inline cmark_parser* parser(bool enabled) {
  cmark_gfm_core_extensions_ensure_registered();
  auto* result = cmark_parser_new(enabled ? CMARK_OPT_FOOTNOTES : CMARK_OPT_DEFAULT);
  for (const auto* name : {"table", "autolink", "tasklist", "math"})
    cmark_parser_attach_syntax_extension(result, cmark_find_syntax_extension(name));
  return result;
}

inline QJsonObject snapshot(cmark_parser* parser, const QByteArray& source, int chunkSize,
                            bool requireOwnedText = false) {
  for (qsizetype offset = 0; offset < source.size(); offset += chunkSize)
    cmark_parser_feed(parser, source.constData() + offset, size_t(qMin(qsizetype(chunkSize), source.size() - offset)));
  auto* root = cmark_parser_finish(parser);
  if (!root || cmark_node_check(root, stderr)) qFatal("Invalid cmark footnote tree");
  QCryptographicHash hash(QCryptographicHash::Sha256);
  auto* iter = cmark_iter_new(root);
  int definitions = 0, references = 0;
  cmark_event_type event;
  bool inLink = false;
  while ((event = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    auto* node = cmark_iter_get_node(iter);
    if (node->type == CMARK_NODE_LINK) inLink = event == CMARK_EVENT_ENTER;
    if (requireOwnedText && !inLink && node->type == CMARK_NODE_TEXT && !node->as.literal.alloc)
      qFatal("Autolink moved a text allocation out of parsing into get_literal()");
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << qint32(event) << QByteArray(cmark_node_get_type_string(node)) << QByteArray(cmark_node_get_literal(node))
        << QByteArray(cmark_node_get_url(node)) << QByteArray(cmark_node_get_title(node))
        << cmark_node_get_start_line(node) << cmark_node_get_start_column(node) << cmark_node_get_end_line(node)
        << cmark_node_get_end_column(node) << cmark_node_get_heading_level(node) << cmark_node_get_list_type(node)
        << cmark_node_get_list_start(node) << cmark_node_get_list_tight(node);
    if (node->type == CMARK_NODE_FOOTNOTE_REFERENCE || node->type == CMARK_NODE_FOOTNOTE_DEFINITION)
      out << node->footnote.ref_ix;
    if (auto* definition = cmark_node_parent_footnote_def(node))
      out << QByteArray(cmark_node_get_literal(definition)) << definition->start_line << definition->start_column;
    hash.addData(bytes);
    if (event == CMARK_EVENT_ENTER) {
      definitions += node->type == CMARK_NODE_FOOTNOTE_DEFINITION;
      references += node->type == CMARK_NODE_FOOTNOTE_REFERENCE;
    }
  }
  cmark_iter_free(iter);
  char* rendered = cmark_render_html(root, CMARK_OPT_DEFAULT, cmark_parser_get_syntax_extensions(parser));
  const auto html = QString::fromUtf8(rendered);
  std::free(rendered);
  cmark_node_free(root);
  return {{"tree_sha256", QString::fromLatin1(hash.result().toHex())},
          {"html", html},
          {"definitions", definitions},
          {"references", references}};
}
}  // namespace footnote_test

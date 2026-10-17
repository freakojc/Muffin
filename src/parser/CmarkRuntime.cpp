#include "parser/CmarkRuntime.h"

#include <mutex>

namespace muffin {
namespace {
std::mutex& inlineParsingMutex() {
  static std::mutex mutex;
  return mutex;
}
}  // namespace

cmark_node* finishCmarkParse(cmark_parser* parser) {
  const std::lock_guard lock(inlineParsingMutex());
  return cmark_parser_finish(parser);
}

cmark_node* parseCmarkDocument(const char* bytes, size_t length, int options) {
  const std::lock_guard lock(inlineParsingMutex());
  return cmark_parse_document(bytes, length, options);
}

}  // namespace muffin

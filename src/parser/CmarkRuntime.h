#pragma once

extern "C" {
#include "cmark-gfm.h"
}

namespace muffin {

// cmark mutates process-wide extension character tables while finishing inline
// parsing, including for parsers without extensions. Keep synchronization in
// Muffin and use only the public cmark API. Callers own the returned tree.
cmark_node* finishCmarkParse(cmark_parser* parser);
cmark_node* parseCmarkDocument(const char* bytes, size_t length, int options);

} // namespace muffin

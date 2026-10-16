// Diagnostic-only translation unit. Compile vendored blocks.c with external
// inline/consolidation calls redirected and two coarse generated phase hooks.
// This object supplies the block-parser
// symbols, so the archive's blocks.o is not linked. The application library
// continues to use its ordinary, uninstrumented cmark archive.
#include "cmark_stage_profile.h"
static muffin_cmark_profile* active_profile;
static uint64_t stage_start[2];
static void muffin_probe_stage_begin(int stage) {
  if (active_profile) stage_start[stage] = muffin_cmark_clock_ns();
}
static void muffin_probe_stage_end(int stage) {
  if (!active_profile) return;
  if (stage == 0)
    active_profile->inline_walk_ns += muffin_cmark_clock_ns() - stage_start[stage];
  else
    active_profile->footnotes_ns += muffin_cmark_clock_ns() - stage_start[stage];
}
#define cmark_parse_inlines muffin_probe_parse_inlines
#define cmark_consolidate_text_nodes muffin_probe_consolidate_text_nodes
#include "iterator.h"
#define cmark_consolidate_text_nodes_with_callback muffin_probe_consolidate_with_callback
void muffin_probe_consolidate_text_nodes(cmark_node* root);
void muffin_probe_consolidate_with_callback(cmark_node*, cmark_consolidated_text_func, void*);
#include "cmark-profiled-blocks.inc"
#undef cmark_parse_inlines
#undef cmark_consolidate_text_nodes
#undef cmark_consolidate_text_nodes_with_callback
#include "cmark_stage_profile.h"

void cmark_parse_inlines(cmark_parser*, cmark_node*, cmark_map*, int);
void cmark_consolidate_text_nodes(cmark_node*);

// The standalone probe runs one parser on one thread. Postprocessors are
// temporarily wrapped and restored before destruction or the next parse.
static cmark_syntax_extension* wrapped_extension;
static cmark_postprocess_func original_postprocess;

void muffin_probe_parse_inlines(cmark_parser* parser, cmark_node* parent, cmark_map* refs, int options) {
  uint64_t start = active_profile ? muffin_cmark_clock_ns() : 0;
  cmark_parse_inlines(parser, parent, refs, options);
  if (active_profile) {
    active_profile->inline_ns += muffin_cmark_clock_ns() - start;
    ++active_profile->inline_calls;
  }
}
void muffin_probe_consolidate_text_nodes(cmark_node* root) {
  uint64_t start = active_profile ? muffin_cmark_clock_ns() : 0;
  cmark_consolidate_text_nodes(root);
  if (active_profile) active_profile->consolidate_ns += muffin_cmark_clock_ns() - start;
}
void muffin_probe_consolidate_with_callback(cmark_node* root, cmark_consolidated_text_func callback, void* data) {
  uint64_t start = active_profile ? muffin_cmark_clock_ns() : 0;
  cmark_consolidate_text_nodes_with_callback(root, callback, data);
  if (active_profile) active_profile->consolidate_ns += muffin_cmark_clock_ns() - start;
}
static cmark_node* profile_postprocess(cmark_syntax_extension* extension, cmark_parser* parser, cmark_node* root) {
  uint64_t start = muffin_cmark_clock_ns();
  cmark_node* result = original_postprocess(extension, parser, root);
  active_profile->postprocess_ns += muffin_cmark_clock_ns() - start;
  ++active_profile->postprocess_calls;
  return result;
}
void muffin_cmark_profile_begin(cmark_parser* parser, muffin_cmark_profile* profile) {
  cmark_llist* extensions;
  assert(!active_profile && !wrapped_extension);
  memset(profile, 0, sizeof(*profile));
  active_profile = profile;
  muffin_cmark_autolink_profile_begin(profile);
  for (extensions = parser->syntax_extensions; extensions; extensions = extensions->next) {
    cmark_syntax_extension* ext = (cmark_syntax_extension*)extensions->data;
    if (ext->postprocess_func) {
      // In the production extension set only autolink has a postprocessor.
      // Reject new hooks instead of silently attributing another extension.
      if (wrapped_extension || strcmp(ext->name, "autolink") != 0) abort();
      wrapped_extension = ext;
      original_postprocess = ext->postprocess_func;
      ext->postprocess_func = profile_postprocess;
    }
  }
}
void muffin_cmark_profile_end(void) {
  muffin_cmark_autolink_profile_end();
  if (wrapped_extension) wrapped_extension->postprocess_func = original_postprocess;
  wrapped_extension = NULL;
  original_postprocess = NULL;
  active_profile = NULL;
}

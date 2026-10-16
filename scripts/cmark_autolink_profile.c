// Diagnostic-only replacement for the archive's autolink.o. No product hooks.
#include "cmark_stage_profile.h"
#include "parser.h"
static muffin_cmark_profile* autolink_profile;
static void muffin_autolink_consolidate(cmark_node* root);
static void muffin_autolink_text(cmark_parser* parser, cmark_node* text);
static void muffin_autolink_visit(void) {
  if (autolink_profile) ++autolink_profile->autolink_visits;
}
#define cmark_consolidate_text_nodes muffin_autolink_consolidate
#include "cmark-profiled-autolink.inc"
#undef cmark_consolidate_text_nodes
void cmark_consolidate_text_nodes(cmark_node* root);
static void muffin_autolink_consolidate(cmark_node* root) {
  uint64_t start = autolink_profile ? muffin_cmark_clock_ns() : 0;
  cmark_consolidate_text_nodes(root);
  if (autolink_profile) autolink_profile->autolink_consolidate_ns += muffin_cmark_clock_ns() - start;
}
static void muffin_autolink_text(cmark_parser* parser, cmark_node* text) {
  uint64_t start = autolink_profile ? muffin_cmark_clock_ns() : 0;
  if (autolink_profile) {
    ++autolink_profile->autolink_text_calls;
    autolink_profile->autolink_text_bytes += text->as.literal.len;
    autolink_profile->autolink_candidates +=
        text->as.literal.len > 0 && memchr(text->as.literal.data, '@', text->as.literal.len) != NULL;
  }
  postprocess_text(parser, text);
  if (autolink_profile) autolink_profile->autolink_text_ns += muffin_cmark_clock_ns() - start;
}
void muffin_cmark_autolink_profile_begin(muffin_cmark_profile* profile) { autolink_profile = profile; }
void muffin_cmark_autolink_profile_end(void) { autolink_profile = NULL; }

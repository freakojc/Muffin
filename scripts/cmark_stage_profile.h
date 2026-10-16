#pragma once
#include <stdint.h>

#include "cmark-gfm.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  uint64_t inline_ns, inline_calls, inline_walk_ns, footnotes_ns, consolidate_ns, postprocess_ns, postprocess_calls;
  uint64_t autolink_consolidate_ns, autolink_text_ns, autolink_text_calls, autolink_text_bytes;
  uint64_t autolink_candidates, autolink_visits;
} muffin_cmark_profile;
uint64_t muffin_cmark_clock_ns(void);
void muffin_cmark_profile_begin(cmark_parser* parser, muffin_cmark_profile* profile);
void muffin_cmark_profile_end(void);
void muffin_cmark_autolink_profile_begin(muffin_cmark_profile* profile);
void muffin_cmark_autolink_profile_end(void);
#ifdef __cplusplus
}
#endif

#ifndef CMARK_ITERATOR_H
#define CMARK_ITERATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cmark-gfm.h"

typedef struct {
  cmark_event_type ev_type;
  cmark_node *node;
} cmark_iter_state;

struct cmark_iter {
  cmark_mem *mem;
  cmark_node *root;
  cmark_iter_state cur;
  cmark_iter_state next;
};

/* Private fused pass: visit each consolidated text node outside existing links.
 * The visitor may split that node, but must not mutate other sibling runs. */
typedef void (*cmark_consolidated_text_func)(cmark_node *text, void *user_data);
void cmark_consolidate_text_nodes_with_callback(cmark_node *root,
                                               cmark_consolidated_text_func callback,
                                               void *user_data);

#ifdef __cplusplus
}
#endif

#endif

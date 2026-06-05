#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdbool.h>
#include "vm/page.h"

void *frame_alloc (struct supplemental_page *);
void frame_free (void *);
void frame_pin (void *);
void frame_unpin (void *);

#endif /* vm/frame.h */

#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdbool.h>
#include <stddef.h>

#define SWAP_ERROR ((size_t) -1)

size_t swap_out (void *);
bool swap_in (size_t, void *);
void swap_free (size_t);

#endif /* vm/swap.h */

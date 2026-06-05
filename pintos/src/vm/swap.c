#include "vm/swap.h"
#include <bitmap.h>
#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

static struct block *swap_block;
static struct bitmap *swap_map;
static struct lock swap_lock;
static bool initialized;

static void
swap_init (void)
{
  if (initialized)
    return;

  swap_block = block_get_role (BLOCK_SWAP);
  if (swap_block != NULL)
    swap_map = bitmap_create (block_size (swap_block) / SECTORS_PER_PAGE);
  lock_init (&swap_lock);
  initialized = true;
}

size_t
swap_out (void *kpage)
{
  size_t idx, i;

  swap_init ();
  if (swap_block == NULL || swap_map == NULL)
    return SWAP_ERROR;

  lock_acquire (&swap_lock);
  idx = bitmap_scan_and_flip (swap_map, 0, 1, false);
  if (idx == BITMAP_ERROR)
    {
      lock_release (&swap_lock);
      return SWAP_ERROR;
    }

  for (i = 0; i < SECTORS_PER_PAGE; i++)
    block_write (swap_block, idx * SECTORS_PER_PAGE + i,
                 (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);
  lock_release (&swap_lock);
  return idx;
}

bool
swap_in (size_t idx, void *kpage)
{
  size_t i;

  swap_init ();
  if (swap_block == NULL || swap_map == NULL)
    return false;

  lock_acquire (&swap_lock);
  if (idx >= bitmap_size (swap_map) || !bitmap_test (swap_map, idx))
    {
      lock_release (&swap_lock);
      return false;
    }

  for (i = 0; i < SECTORS_PER_PAGE; i++)
    block_read (swap_block, idx * SECTORS_PER_PAGE + i,
                (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);
  bitmap_set (swap_map, idx, false);
  lock_release (&swap_lock);
  return true;
}

void
swap_free (size_t idx)
{
  swap_init ();
  if (swap_map == NULL || idx >= bitmap_size (swap_map))
    return;

  lock_acquire (&swap_lock);
  bitmap_set (swap_map, idx, false);
  lock_release (&swap_lock);
}

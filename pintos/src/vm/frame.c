#include "vm/frame.h"
#include <debug.h>
#include <list.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "vm/swap.h"

struct frame
  {
    void *kpage;
    struct thread *owner;
    struct supplemental_page *spte;
    bool pinned;
    struct list_elem elem;
  };

static struct list frame_table;
static struct lock frame_lock;
static struct list_elem *clock_hand;
static bool initialized;

static void
frame_init (void)
{
  if (initialized)
    return;
  list_init (&frame_table);
  lock_init (&frame_lock);
  clock_hand = NULL;
  initialized = true;
}

static struct frame *
frame_lookup (void *kpage)
{
  struct list_elem *e;

  for (e = list_begin (&frame_table); e != list_end (&frame_table);
       e = list_next (e))
    {
      struct frame *f = list_entry (e, struct frame, elem);
      if (f->kpage == kpage)
        return f;
    }
  return NULL;
}

static void *
evict_frame (void)
{
  size_t scanned = 0;

  ASSERT (lock_held_by_current_thread (&frame_lock));
  ASSERT (!list_empty (&frame_table));

  if (clock_hand == NULL || clock_hand == list_end (&frame_table))
    clock_hand = list_begin (&frame_table);

  while (scanned < list_size (&frame_table) * 2 + 1)
    {
      struct frame *f = list_entry (clock_hand, struct frame, elem);
      clock_hand = list_next (clock_hand);
      if (clock_hand == list_end (&frame_table))
        clock_hand = list_begin (&frame_table);
      scanned++;

      if (f->pinned || f->spte->pinned)
        continue;

      if (pagedir_is_accessed (f->owner->pagedir, f->spte->upage))
        {
          pagedir_set_accessed (f->owner->pagedir, f->spte->upage, false);
          continue;
        }

      bool dirty = pagedir_is_dirty (f->owner->pagedir, f->spte->upage);
      if (f->spte->type == PAGE_MMAP)
        {
          if (dirty)
            file_write_at (f->spte->file, f->kpage, f->spte->read_bytes,
                           f->spte->ofs);
        }
      else if (dirty || f->spte->type == PAGE_ZERO || f->spte->type == PAGE_SWAP)
        {
          size_t idx = swap_out (f->kpage);
          if (idx == SWAP_ERROR)
            PANIC ("swap partition is full");
          f->spte->swap_idx = idx;
          f->spte->type = PAGE_SWAP;
        }

      pagedir_clear_page (f->owner->pagedir, f->spte->upage);
      f->spte->loaded = false;
      f->spte->kpage = NULL;
      list_remove (&f->elem);
      void *kpage = f->kpage;
      free (f);
      return kpage;
    }

  PANIC ("no evictable frame");
}

void *
frame_alloc (struct supplemental_page *spte)
{
  struct frame *f;
  void *kpage;

  frame_init ();
  kpage = palloc_get_page (PAL_USER);

  lock_acquire (&frame_lock);
  if (kpage == NULL)
    kpage = evict_frame ();

  f = malloc (sizeof *f);
  if (f == NULL)
    {
      lock_release (&frame_lock);
      palloc_free_page (kpage);
      return NULL;
    }

  f->kpage = kpage;
  f->owner = thread_current ();
  f->spte = spte;
  f->pinned = true;
  spte->pinned = true;
  list_push_back (&frame_table, &f->elem);
  lock_release (&frame_lock);

  return kpage;
}

void
frame_free (void *kpage)
{
  struct frame *f;

  if (kpage == NULL)
    return;
  frame_init ();
  lock_acquire (&frame_lock);
  f = frame_lookup (kpage);
  if (f != NULL)
    {
      list_remove (&f->elem);
      free (f);
    }
  lock_release (&frame_lock);
  palloc_free_page (kpage);
}

void
frame_pin (void *kpage)
{
  struct frame *f;

  frame_init ();
  lock_acquire (&frame_lock);
  f = frame_lookup (kpage);
  if (f != NULL)
    {
      f->pinned = true;
      f->spte->pinned = true;
    }
  lock_release (&frame_lock);
}

void
frame_unpin (void *kpage)
{
  struct frame *f;

  frame_init ();
  lock_acquire (&frame_lock);
  f = frame_lookup (kpage);
  if (f != NULL)
    {
      f->pinned = false;
      f->spte->pinned = false;
    }
  lock_release (&frame_lock);
}

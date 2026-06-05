#include "vm/page.h"
#include <string.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "vm/frame.h"
#include "vm/swap.h"

static struct supplemental_page *page_create (void *);
static void page_remove (struct supplemental_page *);

void
vm_init_thread (struct thread *t)
{
  list_init (&t->supplemental_pages);
  list_init (&t->mmap_list);
  t->next_mapid = 1;
  t->user_esp = PHYS_BASE;
}

struct supplemental_page *
vm_lookup_page (void *addr)
{
  struct thread *cur = thread_current ();
  void *upage = pg_round_down (addr);
  struct list_elem *e;

  for (e = list_begin (&cur->supplemental_pages);
       e != list_end (&cur->supplemental_pages);
       e = list_next (e))
    {
      struct supplemental_page *spte =
        list_entry (e, struct supplemental_page, elem);
      if (spte->upage == upage)
        return spte;
    }
  return NULL;
}

bool
vm_range_free (void *addr, size_t size)
{
  uint8_t *upage = pg_round_down (addr);
  uint8_t *end = pg_round_down ((uint8_t *) addr + size - 1);

  if (addr == NULL || pg_ofs (addr) != 0 || !is_user_vaddr (addr))
    return false;
  for (;;)
    {
      if (vm_lookup_page (upage) != NULL
          || pagedir_get_page (thread_current ()->pagedir, upage) != NULL)
        return false;
      if (upage == end)
        break;
      upage += PGSIZE;
    }
  return true;
}

static struct supplemental_page *
page_create (void *upage)
{
  struct supplemental_page *spte;

  if (upage == NULL || pg_ofs (upage) != 0 || !is_user_vaddr (upage)
      || vm_lookup_page (upage) != NULL)
    return NULL;

  spte = malloc (sizeof *spte);
  if (spte == NULL)
    return NULL;
  memset (spte, 0, sizeof *spte);
  spte->upage = upage;
  spte->swap_idx = SWAP_ERROR;
  list_push_back (&thread_current ()->supplemental_pages, &spte->elem);
  return spte;
}

bool
vm_add_file_page (void *upage, struct file *file, off_t ofs,
                  uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  struct supplemental_page *spte = page_create (upage);
  if (spte == NULL)
    return false;
  spte->type = PAGE_FILE;
  spte->file = file;
  spte->ofs = ofs;
  spte->read_bytes = read_bytes;
  spte->zero_bytes = zero_bytes;
  spte->writable = writable;
  return true;
}

bool
vm_add_zero_page (void *upage, bool writable)
{
  struct supplemental_page *spte = page_create (upage);
  if (spte == NULL)
    return false;
  spte->type = PAGE_ZERO;
  spte->read_bytes = 0;
  spte->zero_bytes = PGSIZE;
  spte->writable = writable;
  return true;
}

bool
vm_load_page (void *addr)
{
  struct supplemental_page *spte = vm_lookup_page (addr);
  uint8_t *kpage;
  bool ok = false;

  if (spte == NULL)
    return false;
  if (spte->loaded)
    return true;

  kpage = frame_alloc (spte);
  if (kpage == NULL)
    return false;

  switch (spte->type)
    {
    case PAGE_FILE:
    case PAGE_MMAP:
      lock_acquire (&filesys_lock);
      if (file_read_at (spte->file, kpage, spte->read_bytes, spte->ofs)
          == (int) spte->read_bytes)
        {
          memset (kpage + spte->read_bytes, 0, spte->zero_bytes);
          ok = true;
        }
      lock_release (&filesys_lock);
      break;

    case PAGE_ZERO:
      memset (kpage, 0, PGSIZE);
      ok = true;
      break;

    case PAGE_SWAP:
      ok = swap_in (spte->swap_idx, kpage);
      if (ok)
        spte->type = PAGE_ZERO;
      spte->swap_idx = SWAP_ERROR;
      break;
    }

  if (ok)
    ok = pagedir_set_page (thread_current ()->pagedir, spte->upage, kpage,
                           spte->writable);

  if (!ok)
    {
      frame_free (kpage);
      return false;
    }

  spte->loaded = true;
  spte->kpage = kpage;
  frame_unpin (kpage);
  return true;
}

bool
vm_stack_growth (void *addr)
{
  void *upage = pg_round_down (addr);
  if ((uint8_t *) PHYS_BASE - (uint8_t *) upage > STACK_MAX)
    return false;
  if (!vm_add_zero_page (upage, true))
    return false;
  return vm_load_page (upage);
}

bool
vm_handle_fault (void *fault_addr, void *esp, bool not_present)
{
  uint8_t *addr = fault_addr;

  if (!not_present || fault_addr == NULL || !is_user_vaddr (fault_addr))
    return false;

  if (vm_lookup_page (fault_addr) != NULL)
    return vm_load_page (fault_addr);

  if (addr >= (uint8_t *) esp - 32
      && addr < (uint8_t *) PHYS_BASE
      && (uint8_t *) PHYS_BASE - (uint8_t *) pg_round_down (addr) <= STACK_MAX)
    return vm_stack_growth (fault_addr);

  return false;
}

void
vm_pin_buffer (const void *buffer, size_t size)
{
  uint8_t *start = pg_round_down (buffer);
  uint8_t *end;

  if (size == 0)
    return;
  end = pg_round_down ((const uint8_t *) buffer + size - 1);
  for (;;)
    {
      if (pagedir_get_page (thread_current ()->pagedir, start) == NULL)
        vm_handle_fault (start, thread_current ()->user_esp, true);
      if (pagedir_get_page (thread_current ()->pagedir, start) != NULL)
        frame_pin (pagedir_get_page (thread_current ()->pagedir, start));
      if (start == end)
        break;
      start += PGSIZE;
    }
}

void
vm_unpin_buffer (const void *buffer, size_t size)
{
  uint8_t *start = pg_round_down (buffer);
  uint8_t *end;

  if (size == 0)
    return;
  end = pg_round_down ((const uint8_t *) buffer + size - 1);
  for (;;)
    {
      if (pagedir_get_page (thread_current ()->pagedir, start) != NULL)
        frame_unpin (pagedir_get_page (thread_current ()->pagedir, start));
      if (start == end)
        break;
      start += PGSIZE;
    }
}

mapid_t
vm_mmap (int fd, void *addr)
{
  struct file_desc *desc;
  struct file *file;
  struct mmap_file *mapping;
  off_t length;
  off_t ofs = 0;
  uint8_t *upage = addr;
  mapid_t mapid;

  if (fd < 3 || addr == NULL || pg_ofs (addr) != 0)
    return MAP_FAILED;

  desc = find_file_desc (thread_current (), fd);
  if (desc == NULL || desc->file == NULL)
    return MAP_FAILED;

  lock_acquire (&filesys_lock);
  length = file_length (desc->file);
  file = file_reopen (desc->file);
  lock_release (&filesys_lock);
  if (length <= 0 || file == NULL)
    return MAP_FAILED;

  if (!vm_range_free (addr, length))
    {
      file_close (file);
      return MAP_FAILED;
    }

  mapping = malloc (sizeof *mapping);
  if (mapping == NULL)
    {
      file_close (file);
      return MAP_FAILED;
    }
  mapid = thread_current ()->next_mapid++;
  mapping->mapid = mapid;
  mapping->file = file;
  list_push_back (&thread_current ()->mmap_list, &mapping->elem);

  while (length > 0)
    {
      uint32_t read_bytes = length < PGSIZE ? length : PGSIZE;
      uint32_t zero_bytes = PGSIZE - read_bytes;
      struct supplemental_page *spte;

      spte = page_create (upage);
      if (spte == NULL)
        {
          vm_munmap (mapid);
          return MAP_FAILED;
        }
      spte->type = PAGE_MMAP;
      spte->file = file;
      spte->ofs = ofs;
      spte->read_bytes = read_bytes;
      spte->zero_bytes = zero_bytes;
      spte->writable = true;
      spte->mapid = mapid;

      length -= read_bytes;
      ofs += read_bytes;
      upage += PGSIZE;
    }

  return mapid;
}

void
vm_munmap (mapid_t mapid)
{
  struct thread *cur = thread_current ();
  struct list_elem *e, *next;
  struct mmap_file *mapping = NULL;

  for (e = list_begin (&cur->mmap_list); e != list_end (&cur->mmap_list);
       e = list_next (e))
    {
      struct mmap_file *m = list_entry (e, struct mmap_file, elem);
      if (m->mapid == mapid)
        {
          mapping = m;
          break;
        }
    }
  if (mapping == NULL)
    return;

  for (e = list_begin (&cur->supplemental_pages);
       e != list_end (&cur->supplemental_pages); e = next)
    {
      struct supplemental_page *spte =
        list_entry (e, struct supplemental_page, elem);
      next = list_next (e);
      if (spte->type == PAGE_MMAP && spte->mapid == mapid)
        page_remove (spte);
    }

  list_remove (&mapping->elem);
  file_close (mapping->file);
  free (mapping);
}

static void
page_remove (struct supplemental_page *spte)
{
  if (spte->loaded)
    {
      bool dirty = pagedir_is_dirty (thread_current ()->pagedir, spte->upage);
      if (spte->type == PAGE_MMAP && dirty)
        {
          lock_acquire (&filesys_lock);
          file_write_at (spte->file, spte->kpage, spte->read_bytes, spte->ofs);
          lock_release (&filesys_lock);
        }
      pagedir_clear_page (thread_current ()->pagedir, spte->upage);
      frame_free (spte->kpage);
    }
  else if (spte->type == PAGE_SWAP && spte->swap_idx != SWAP_ERROR)
    swap_free (spte->swap_idx);
  list_remove (&spte->elem);
  free (spte);
}

void
vm_destroy_thread (struct thread *t)
{
  struct list_elem *e, *next;

  while (!list_empty (&t->mmap_list))
    {
      struct mmap_file *m = list_entry (list_front (&t->mmap_list),
                                        struct mmap_file, elem);
      vm_munmap (m->mapid);
    }

  for (e = list_begin (&t->supplemental_pages);
       e != list_end (&t->supplemental_pages); e = next)
    {
      struct supplemental_page *spte =
        list_entry (e, struct supplemental_page, elem);
      next = list_next (e);
      page_remove (spte);
    }
}

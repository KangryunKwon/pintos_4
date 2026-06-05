#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdbool.h>
#include <list.h>
#include "filesys/file.h"
#include "threads/thread.h"

#define STACK_MAX (8 * 1024 * 1024)
#define MAP_FAILED ((mapid_t) -1)

typedef int mapid_t;

enum page_type
  {
    PAGE_FILE,
    PAGE_ZERO,
    PAGE_SWAP,
    PAGE_MMAP
  };

struct supplemental_page
  {
    void *upage;
    enum page_type type;
    struct file *file;
    off_t ofs;
    uint32_t read_bytes;
    uint32_t zero_bytes;
    bool writable;
    bool loaded;
    bool pinned;
    void *kpage;
    size_t swap_idx;
    mapid_t mapid;
    struct list_elem elem;
  };

struct mmap_file
  {
    mapid_t mapid;
    struct file *file;
    struct list_elem elem;
  };

void vm_init_thread (struct thread *);
void vm_destroy_thread (struct thread *);
struct supplemental_page *vm_lookup_page (void *);
bool vm_add_file_page (void *, struct file *, off_t, uint32_t, uint32_t, bool);
bool vm_add_zero_page (void *, bool);
bool vm_load_page (void *);
bool vm_stack_growth (void *);
bool vm_handle_fault (void *fault_addr, void *esp, bool not_present);
void vm_pin_buffer (const void *, size_t);
void vm_unpin_buffer (const void *, size_t);
mapid_t vm_mmap (int fd, void *addr);
void vm_munmap (mapid_t mapid);
bool vm_range_free (void *, size_t);

#endif /* vm/page.h */

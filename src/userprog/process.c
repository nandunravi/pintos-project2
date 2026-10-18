#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"

/* ELF types. See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* Executable header. See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header. See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff. */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type. See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags. See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

#define MAX_ARGS 64

/* Auxiliary structure to pass startup parameters safely to child thread */
struct exec_args {
  char *file_name;
  struct thread *parent;
  struct child_status *cs;
};

/* Forward declarations */
static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static void push_args (int argc, char **argv, void **esp);
static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);
static bool install_page (void *upage, void *kpage, bool writable);

/* Starts a new thread running a user program loaded from FILE_NAME. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char *name_copy;
  char *prog_name;
  char *save_ptr;
  tid_t tid;

  /* Make a copy of FILE_NAME for tokenization in child thread */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Extract actual executable name for thread naming. Keep the
     page pointer (name_copy) separate from the token pointer
     (prog_name) so that we free the right address. */
  name_copy = palloc_get_page (0);
  if (name_copy == NULL) {
    palloc_free_page (fn_copy);
    return TID_ERROR;
  }
  strlcpy (name_copy, file_name, PGSIZE);
  prog_name = strtok_r (name_copy, " ", &save_ptr);
  if (prog_name == NULL)
    prog_name = name_copy;

  /* Allocate child_status node to track exit status and wait state */
  struct child_status *cs = malloc (sizeof (struct child_status));
  if (cs == NULL) {
    palloc_free_page (fn_copy);
    palloc_free_page (name_copy);
    return TID_ERROR;
  }

  cs->exit_code = -1;
  cs->is_alive = true;
  cs->parent_alive = true;
  cs->parent_waited = false;
  sema_init (&cs->sema_wait, 0);

  struct exec_args args;
  args.file_name = fn_copy;
  args.parent = thread_current ();
  args.cs = cs;

  /* Create new thread */
  tid = thread_create (prog_name, PRI_DEFAULT, start_process, &args);
  palloc_free_page (name_copy);

  if (tid == TID_ERROR) {
    palloc_free_page (fn_copy);
    free (cs);
    return TID_ERROR;
  }

  cs->child_tid = tid;
  list_push_back (&thread_current ()->child_list, &cs->elem);

  /* Block parent until child finishes loading ELF binary */
  sema_down (&thread_current ()->load_sema);

  if (!thread_current ()->load_success) {
    /* Child set its own child_status to NULL before waking us,
       so it is safe to free cs here. */
    list_remove (&cs->elem);
    free (cs);
    return -1;
  }

  return tid;
}

/* A thread function that loads a user process and starts it executing. */
static void
start_process (void *aux_)
{
  struct exec_args *args = aux_;
  char *file_name = args->file_name;
  struct thread *parent = args->parent;
  struct child_status *cs = args->cs;

  struct thread *cur = thread_current ();
  cur->child_status = cs;

  /* Tokenize command line into argument vector */
  char *argv[MAX_ARGS];
  int argc = 0;
  char *token, *save_ptr;

  for (token = strtok_r (file_name, " ", &save_ptr);
       token != NULL && argc < MAX_ARGS - 1;
       token = strtok_r (NULL, " ", &save_ptr)) {
    argv[argc++] = token;
  }
  argv[argc] = NULL;

  struct intr_frame if_;
  bool success = false;

  /* Initialize interrupt frame and load executable binary */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  if (argc > 0)
    success = load (argv[0], &if_.eip, &if_.esp);

  if (success) {
    /* Set up stack with arguments according to 80x86 ABI */
    push_args (argc, argv, &if_.esp);
  }

  palloc_free_page (file_name);

  if (!success) {
    /* The parent frees cs when load fails, so we must not touch it
       again (process_exit prints -1 when child_status is NULL). */
    cur->child_status = NULL;
  }

  /* Signal parent with load status */
  parent->load_success = success;
  sema_up (&parent->load_sema);

  if (!success) 
    sys_exit (-1);

  /* Start the user process via interrupt return */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Pushes command-line arguments onto user stack following x86 calling convention */
static void
push_args (int argc, char **argv, void **esp)
{
  uint32_t arg_addr[MAX_ARGS];

  /* 1. Push argument strings in reverse order */
  for (int i = argc - 1; i >= 0; i--) {
    size_t len = strlen (argv[i]) + 1;
    *esp -= len;
    memcpy (*esp, argv[i], len);
    arg_addr[i] = (uint32_t) *esp;
  }

  /* 2. Word alignment padding to 4-byte boundary */
  while ((uint32_t) *esp % 4 != 0) {
    *esp -= 1;
    *(uint8_t *) *esp = 0;
  }

  /* 3. Null pointer sentinel (argv[argc]) */
  *esp -= sizeof (char *);
  *(char **) *esp = NULL;

  /* 4. Push addresses of argument strings */
  for (int i = argc - 1; i >= 0; i--) {
    *esp -= sizeof (char *);
    *(uint32_t *) *esp = arg_addr[i];
  }

  /* 5. Push argv pointer */
  uint32_t argv_ptr = (uint32_t) *esp;
  *esp -= sizeof (char **);
  *(uint32_t *) *esp = argv_ptr;

  /* 6. Push argc */
  *esp -= sizeof (int);
  *(int *) *esp = argc;

  /* 7. Push dummy return address (0) */
  *esp -= sizeof (void *);
  *(void **) *esp = NULL;
}

/* Waits for child process process_wait() */
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct child_status *child = NULL;
  struct list_elem *e;

  /* Search for child process in parent's list */
  for (e = list_begin (&cur->child_list); e != list_end (&cur->child_list); e = list_next (e)) {
    struct child_status *cs = list_entry (e, struct child_status, elem);
    if (cs->child_tid == child_tid) {
      child = cs;
      break;
    }
  }

  /* Fail if non-child, invalid PID, or already waited */
  if (child == NULL || child->parent_waited) {
    return -1;
  }

  child->parent_waited = true;

  /* Block parent until child thread exits */
  if (child->is_alive) {
    sema_down (&child->sema_wait);
  }

  int exit_code = child->exit_code;
  list_remove (&child->elem);
  free (child);

  return exit_code;
}

/* Free the current process's resources upon termination */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Close all open file descriptors */
  for (int i = 2; i < FD_MAX; i++) {
    if (cur->fd_table[i] != NULL) {
      lock_acquire (&filesys_lock);
      file_close (cur->fd_table[i]);
      lock_release (&filesys_lock);
      cur->fd_table[i] = NULL;
    }
  }

  /* Re-enable writes to executable file and close it */
  if (cur->exec_file != NULL) {
    lock_acquire (&filesys_lock);
    file_allow_write (cur->exec_file);
    file_close (cur->exec_file);
    lock_release (&filesys_lock);
    cur->exec_file = NULL;
  }

  /* Print required process exit status message */
  printf ("%s: exit(%d)\n", cur->name, cur->child_status ? cur->child_status->exit_code : -1);

  /* Update own child_status node and unblock parent if waiting */
  if (cur->child_status != NULL) {
    cur->child_status->is_alive = false;
    if (!cur->child_status->parent_alive) {
      free (cur->child_status);
    } else {
      sema_up (&cur->child_status->sema_wait);
    }
  }

  /* Notify child processes that parent is exiting */
  struct list_elem *e = list_begin (&cur->child_list);
  while (e != list_end (&cur->child_list)) {
    struct child_status *cs = list_entry (e, struct child_status, elem);
    e = list_remove (e);
    if (!cs->is_alive) {
      free (cs);
    } else {
      cs->parent_alive = false;
    }
  }

  /* Destroy page directory */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

void
process_activate (void)
{
  struct thread *t = thread_current ();

  pagedir_activate (t->pagedir);
  tss_update ();
}

/* Loads an ELF executable from FILE_NAME into the current thread. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Synchronize file system access and open executable */
  lock_acquire (&filesys_lock);
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      lock_release (&filesys_lock);
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Deny write on executable file while running */
  file_deny_write (file);
  t->exec_file = file;
  lock_release (&filesys_lock);

  /* Read and verify ELF header */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack */
  if (!setup_stack (esp))
    goto done;

  *eip = (void (*) (void)) ehdr.e_entry;
  success = true;

 done:
  /* On failure, release executable lock and close file */
  if (!success && file != NULL) 
    {
      lock_acquire (&filesys_lock);
      file_allow_write (file);
      file_close (file);
      lock_release (&filesys_lock);
      t->exec_file = NULL;
    }
  return success;
}

static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 
  if (phdr->p_memsz == 0)
    return false;
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;
  if (phdr->p_vaddr < PGSIZE)
    return false;

  return true;
}

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
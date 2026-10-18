#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

/* Global lock to synchronize concurrent file system operations */
struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);
static void check_valid_ptr (const void *vaddr);
static void check_valid_buffer (const void *buffer, unsigned size);
static void check_valid_string (const char *str);
static int get_user_arg (uint32_t *esp, int offset);

/* System call handler helper function prototypes */
static void sys_halt (void);
static pid_t sys_exec (const char *cmd_line);
static int sys_wait (pid_t pid);
static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned size);
static int sys_write (int fd, const void *buffer, unsigned size);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static void sys_close (int fd);

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  check_valid_ptr (f->esp);

  int syscall_num = get_user_arg ((uint32_t *) f->esp, 0);

  switch (syscall_num) {
    case SYS_HALT:
      sys_halt ();
      break;

    case SYS_EXIT:
      sys_exit (get_user_arg ((uint32_t *) f->esp, 1));
      break;

    case SYS_EXEC:
      f->eax = sys_exec ((const char *) get_user_arg ((uint32_t *) f->esp, 1));
      break;

    case SYS_WAIT:
      f->eax = sys_wait ((tid_t) get_user_arg ((uint32_t *) f->esp, 1));
      break;

    case SYS_CREATE:
      f->eax = sys_create ((const char *) get_user_arg ((uint32_t *) f->esp, 1),
                          (unsigned) get_user_arg ((uint32_t *) f->esp, 2));
      break;

    case SYS_REMOVE:
      f->eax = sys_remove ((const char *) get_user_arg ((uint32_t *) f->esp, 1));
      break;

    case SYS_OPEN:
      f->eax = sys_open ((const char *) get_user_arg ((uint32_t *) f->esp, 1));
      break;

    case SYS_FILESIZE:
      f->eax = sys_filesize (get_user_arg ((uint32_t *) f->esp, 1));
      break;

    case SYS_READ:
      f->eax = sys_read (get_user_arg ((uint32_t *) f->esp, 1),
                        (void *) get_user_arg ((uint32_t *) f->esp, 2),
                        (unsigned) get_user_arg ((uint32_t *) f->esp, 3));
      break;

    case SYS_WRITE:
      f->eax = sys_write (get_user_arg ((uint32_t *) f->esp, 1),
                         (const void *) get_user_arg ((uint32_t *) f->esp, 2),
                         (unsigned) get_user_arg ((uint32_t *) f->esp, 3));
      break;

    case SYS_SEEK:
      sys_seek (get_user_arg ((uint32_t *) f->esp, 1),
               (unsigned) get_user_arg ((uint32_t *) f->esp, 2));
      break;

    case SYS_TELL:
      f->eax = sys_tell (get_user_arg ((uint32_t *) f->esp, 1));
      break;

    case SYS_CLOSE:
      sys_close (get_user_arg ((uint32_t *) f->esp, 1));
      break;

    default:
      sys_exit (-1);
      break;
  }
}

/* Validates a single user virtual memory pointer */
static void
check_valid_ptr (const void *vaddr)
{
  if (vaddr == NULL || !is_user_vaddr (vaddr) || pagedir_get_page (thread_current ()->pagedir, vaddr) == NULL) {
    sys_exit (-1);
  }
}

/* Validates an entire buffer region in user virtual memory */
static void
check_valid_buffer (const void *buffer, unsigned size)
{
  check_valid_ptr (buffer);
  const char *ptr = (const char *) buffer;
  for (unsigned i = 0; i < size; i++) {
    check_valid_ptr (ptr + i);
  }
}

/* Validates a null-terminated string residing in user memory */
static void
check_valid_string (const char *str)
{
  check_valid_ptr (str);
  while (*str != '\0') {
    str++;
    check_valid_ptr (str);
  }
}

/* Safely retrieves an argument from the user stack at the given offset */
static int
get_user_arg (uint32_t *esp, int offset)
{
  check_valid_ptr ((const void *) (esp + offset));
  check_valid_ptr ((const void *) ((char *) (esp + offset) + 3));
  return esp[offset];
}

/* 1. SYS_HALT: Terminates Pintos */
static void
sys_halt (void) {
  shutdown_power_off ();
}

/* 2. SYS_EXIT: Terminates current process with status code */
void
sys_exit (int status) {
  struct thread *cur = thread_current ();
  if (cur->child_status != NULL) {
    cur->child_status->exit_code = status;
  }
  thread_exit ();
}

/* 3. SYS_EXEC: Executes a process with command-line arguments */
static pid_t
sys_exec (const char *cmd_line) {
  check_valid_string (cmd_line);
  return process_execute (cmd_line);
}

/* 4. SYS_WAIT: Waits for child process termination */
static int
sys_wait (pid_t pid) {
  return process_wait (pid);
}

/* 5. SYS_CREATE: Creates a file in the file system */
static bool
sys_create (const char *file, unsigned initial_size) {
  check_valid_string (file);
  lock_acquire (&filesys_lock);
  bool success = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  return success;
}

/* 6. SYS_REMOVE: Deletes a file from the file system */
static bool
sys_remove (const char *file) {
  check_valid_string (file);
  lock_acquire (&filesys_lock);
  bool success = filesys_remove (file);
  lock_release (&filesys_lock);
  return success;
}

/* 7. SYS_OPEN: Opens a file and returns an assigned file descriptor (>= 2) */
static int
sys_open (const char *file) {
  check_valid_string (file);
  lock_acquire (&filesys_lock);
  struct file *f = filesys_open (file);
  lock_release (&filesys_lock);

  if (f == NULL) return -1;

  struct thread *cur = thread_current ();
  for (int i = 2; i < FD_MAX; i++) {
    if (cur->fd_table[i] == NULL) {
      cur->fd_table[i] = f;
      return i;
    }
  }

  lock_acquire (&filesys_lock);
  file_close (f);
  lock_release (&filesys_lock);
  return -1;
}

/* 8. SYS_FILESIZE: Returns size in bytes of an open file descriptor */
static int
sys_filesize (int fd) {
  if (fd < 2 || fd >= FD_MAX) return -1;
  struct thread *cur = thread_current ();
  struct file *f = cur->fd_table[fd];
  if (f == NULL) return -1;

  lock_acquire (&filesys_lock);
  int size = file_length (f);
  lock_release (&filesys_lock);
  return size;
}

/* 9. SYS_READ: Reads from stdin (0) or open file descriptor */
static int
sys_read (int fd, void *buffer, unsigned size) {
  check_valid_buffer (buffer, size);

  if (fd == 0) {
    uint8_t *buf = (uint8_t *) buffer;
    for (unsigned i = 0; i < size; i++) {
      buf[i] = input_getc ();
    }
    return size;
  }

  if (fd < 2 || fd >= FD_MAX) return -1;
  struct thread *cur = thread_current ();
  struct file *f = cur->fd_table[fd];
  if (f == NULL) return -1;

  lock_acquire (&filesys_lock);
  int bytes_read = file_read (f, buffer, size);
  lock_release (&filesys_lock);
  return bytes_read;
}

/* 10. SYS_WRITE: Writes to stdout (1) or open file descriptor */
static int
sys_write (int fd, const void *buffer, unsigned size) {
  check_valid_buffer (buffer, size);

  if (fd == 1) {
    putbuf (buffer, size);
    return size;
  }

  if (fd < 2 || fd >= FD_MAX) return -1;
  struct thread *cur = thread_current ();
  struct file *f = cur->fd_table[fd];
  if (f == NULL) return -1;

  lock_acquire (&filesys_lock);
  int bytes_written = file_write (f, buffer, size);
  lock_release (&filesys_lock);
  return bytes_written;
}

/* 11. SYS_SEEK: Moves offset position in open file */
static void
sys_seek (int fd, unsigned position) {
  if (fd < 2 || fd >= FD_MAX) return;
  struct thread *cur = thread_current ();
  struct file *f = cur->fd_table[fd];
  if (f == NULL) return;

  lock_acquire (&filesys_lock);
  file_seek (f, position);
  lock_release (&filesys_lock);
}

/* 12. SYS_TELL: Returns current byte offset position in open file */
static unsigned
sys_tell (int fd) {
  if (fd < 2 || fd >= FD_MAX) return 0;
  struct thread *cur = thread_current ();
  struct file *f = cur->fd_table[fd];
  if (f == NULL) return 0;

  lock_acquire (&filesys_lock);
  unsigned pos = file_tell (f);
  lock_release (&filesys_lock);
  return pos;
}

/* 13. SYS_CLOSE: Closes an open file descriptor */
static void
sys_close (int fd) {
  if (fd < 2 || fd >= FD_MAX) return;
  struct thread *cur = thread_current ();
  struct file *f = cur->fd_table[fd];
  if (f == NULL) return;

  lock_acquire (&filesys_lock);
  file_close (f);
  lock_release (&filesys_lock);
  cur->fd_table[fd] = NULL;
}
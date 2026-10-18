#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"

/* Forward declaration to avoid circular dependency with threads/synch.h */
struct lock;

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* 4.4BSD Scheduler defaults. */
#define NICE_DEFAULT 0
#define RECENT_CPU_DEFAULT 0
#define LOAD_AVG_DEFAULT 0

#define FD_MAX 128                      /* Maximum per-process file descriptors */

/* Dynamic structure tracking child process state for wait and exit status */
struct child_status {
    tid_t child_tid;                    /* Process ID of the child */
    int exit_code;                      /* Exit status code returned by child */
    bool is_alive;                      /* True if child thread is executing */
    bool parent_alive;                  /* True if parent thread is executing */
    bool parent_waited;                 /* True if parent has called wait on child */
    struct semaphore sema_wait;         /* Semaphore to block parent in process_wait */
    struct list_elem elem;              /* List element for parent's child_list */
};

struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    int priority;                       /* Current effective priority. */
    int base_priority;                  /* Base priority before donations. */
    struct list locks_held;             /* Locks held by this thread. */
    struct lock *wait_on_lock;          /* Lock this thread is blocked on. */
    int64_t wakeup_tick;                /* Tick count to wake up from sleep. */

    /* Advanced Scheduler (MLFQS). */
    int nice;                           /* Nice value (-20 to 20). */
    int recent_cpu;                     /* Fixed-point recent CPU usage. */

    struct list_elem allelem;           /* List element for all threads list. */
    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
    struct file *exec_file;             /* Pointer to executable file (to deny write) */
    struct list child_list;             /* List of child_status structures */
    struct child_status *child_status;  /* Pointer to own child_status tracking node */
    struct file *fd_table[FD_MAX];      /* File descriptor table */
    struct semaphore load_sema;         /* Semaphore to synchronize process loading */
    bool load_success;                  /* Execution load status */
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* Priority helper functions. */
bool thread_priority_less (const struct list_elem *a,
                           const struct list_elem *b,
                           void *aux);
void test_max_priority (void);

/* MLFQS recalculation functions used across timer and scheduler. */
void mlfqs_calculate_priority (struct thread *t);
void mlfqs_calculate_recent_cpu (struct thread *t);
void mlfqs_calculate_load_avg (void);
void mlfqs_recalculate_all (void);

/* Global structures shared across thread.c and timer.c */
extern struct list all_list;
extern struct thread *idle_thread;

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /* threads/thread.h */
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define TARGET_LATENCY 48    // Target latency in ticks
#define MIN_TIME_SLICE 3     // Minimum time slice
#define NICE_0_WEIGHT 1024   // Weight for nice value 0

struct cpu cpus[NCPU];

int scheduler_logging_enabled=0; // Default: logging disabled

// Calculate weight based on nice value: weight = 1024 / (1.25 ^ nice)
uint64 calculate_weight(int nice) {
  if (nice == 0) return NICE_0_WEIGHT;

  // Pre-calculated weights for efficiency (approximation of 1024 / 1.25^nice)
  static uint64 weights[] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23283, 18626, 14996, 11916,
    9548, 7620, 6100, 4904, 3906, 3121, 2501, 1991, 1586, 1277,
    1024, 820, 655, 526, 423, 335, 272, 215, 172, 137,
    110, 87, 70, 56, 45, 36, 29, 23, 18, 15
  };

  int index = nice + 20; // Convert nice range [-20, 19] to [0, 39]
  if (index < 0) index = 0;
  if (index >= 40) index = 39;

  return weights[index];
}

#ifdef CFS
#define WEIGHT_TO_TIME_SLICE(weight) (48 / (weight)) // Example calculation
#endif

#ifdef CFS
uint64
calculate_time_slice(int runnable_count)
{
  if (runnable_count <= 0) {
    return 48; // Default time slice if no runnable processes
  }
  return 48 / runnable_count; // Divide the total time slice among runnable processes
}
#endif

#ifdef CFS
static uint weights[] = {
    88761, 71755, 59560, 49266, 40488, 33177, 27245, 22287, 18274, 15000,
    12295, 10085, 8285, 6810, 5589, 4589, 3768, 3096, 2540, 2085,
    1712, 1406, 1156, 950, 780, 640, 526, 432, 355, 292,
    240, 197, 162, 133, 109, 89, 74, 61, 50, 41,
    34, 28, 23, 19, 15
};

static uint64 min_vruntime = 0;

int
nice_to_weight(int nice)
{
  // Map the nice value (e.g., -20) to the correct array index (e.g., 0).
  return weights[nice + 20];
}
#endif

extern uint ticks;

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

#ifdef FCFS
  p->ctime = ticks;
#endif

#ifdef CFS
  p->nice = 0; // Default nice value
  p->weight = calculate_weight(p->nice); // Initialize weight based on nice value
  p->vruntime = min_vruntime; // Initialize vRuntime to the global minimum
#endif

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(np->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();
    
    int found = 0;
    
#ifdef FCFS
     // FCFS scheduler
     struct proc *earliest_p = 0;

     // Find the process with the earliest creation time (p->ctime)
     for (p = proc; p < &proc[NPROC]; p++) {
       acquire(&p->lock);
       if (p->state == RUNNABLE) {
         if (earliest_p == 0 || p->ctime < earliest_p->ctime) {
           if (earliest_p) {
             release(&earliest_p->lock);
           }
           earliest_p = p;
         } else {
           release(&p->lock);
         }
       } else {
         release(&p->lock);
       }
     }
 
     // If a process is found, run it to completion
     if (earliest_p) {
       p = earliest_p;
       p->state = RUNNING;
       c->proc = p;
 
       // Switch to the selected process
       swtch(&c->context, &p->context);
 
       // After the process finishes, it will either exit or yield.
       c->proc = 0;
 
       // Ensure the process is no longer RUNNING before releasing the lock
       if (p->state == RUNNING) {
         p->state = RUNNABLE;
       }
 
       release(&p->lock);
       found = 1;
     }
#elif defined(CFS)
#define TARGET_LATENCY 48    // Target latency in ticks
#define MIN_TIME_SLICE 3     // Minimum time slice
#define NICE_0_WEIGHT 1024   // Weight for nice value 0

    struct proc *min_vruntime_proc = 0;
    int runnable_procs = 0;
    uint64 total_weight = 0;

    // 1. Find the process with the smallest vRuntime
    for (p = proc; p < &proc[NPROC]; p++) {
      if (p->state == RUNNABLE) {
        runnable_procs++;
        total_weight += p->weight;
        if (min_vruntime_proc == 0 || p->vruntime < min_vruntime_proc->vruntime) {
          min_vruntime_proc = p;
        }
      }
    }

    if (min_vruntime_proc) {
      p = min_vruntime_proc;
      acquire(&p->lock);

      if (p->state == RUNNABLE) {
        // Calculate the time slice based on the process's weight
        int time_slice = (TARGET_LATENCY * p->weight) / total_weight;
        if (time_slice < MIN_TIME_SLICE) {
          time_slice = MIN_TIME_SLICE; // Minimum time slice
        }
        p->time_slice = time_slice;
        p->tick_count = 0;

        // Log the scheduler decision
        if (scheduler_logging_enabled) {
          printf("[Scheduler Tick] (Runnable: %d, Time Slice: %d ticks)\n", runnable_procs, time_slice);
          for (struct proc *q = proc; q < &proc[NPROC]; q++) {
            if (q->state == RUNNABLE) {
              printf("PID: %d | Nice: %d | Weight: %d | vRuntime: %lu | TimeSlice: %d\n",
                     q->pid, q->nice, q->weight, q->vruntime, q->time_slice);
            }
          }
          printf("--> Scheduling PID %d (lowest vRuntime: %lu)\n", p->pid, p->vruntime);
        }

        // Switch to the chosen process
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process has yielded
        c->proc = 0;
      }

      release(&p->lock);
    }

    // Update the global minimum vRuntime
    if (min_vruntime_proc) {
      min_vruntime = min_vruntime_proc->vruntime;
    }
#else // default round-robin
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
#endif
    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;

#ifdef CFS
  // Update the process's vRuntime based on its weight
  uint64 delta_exec = p->tick_count * (NICE_0_WEIGHT / p->weight); // Normalize by weight
  p->vruntime += delta_exec;
  p->tick_count = 0; // Reset tick count
#endif

  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
#ifdef CFS
    acquire(&p->lock);
    p->vruntime = min_vruntime;
    release(&p->lock);
#endif
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console. For debugging CFS.
// Runs when user types ^P on console.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused  ",
  [SLEEPING]  "sleeping",
  [RUNNABLE]  "runnable",
  [RUNNING]   "running ",
  [ZOMBIE]    "zombie  "
  };
  struct proc *p;
  char *state;

#ifdef CFS
  // Print the detailed header for the CFS scheduler.
  printf("\nPID\tSTATE   \tNICE\tWEIGHT\tVRUNTIME\tSLICE\t\tNAME\n");
  printf("--------------------------------------------------------------------------\n");
#else
  printf("\n"); // For non-CFS schedulers, just print a newline.
#endif
  
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

#ifdef CFS
    // This is the correct, detailed printf for the CFS scheduler.
    printf("%d\t%s\t%d\t%d\t%lu\t\t%d/%d\t\t%s\n", 
           p->pid, 
           state, 
           p->nice, 
           nice_to_weight(p->nice),
           p->vruntime, 
           p->tick_count, 
           p->time_slice, 
           p->name);
#else
    // This is the original, basic printf for other schedulers.
    printf("%d %s %s\n", p->pid, state, p->name);
#endif
  }
}
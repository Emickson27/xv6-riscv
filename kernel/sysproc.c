#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"


extern struct proc proc[NPROC];


uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

// returns child process status and rusage
uint64
sys_wait2(void)
{
  uint64 p;
  uint64 rusage_p;
  argaddr(0, &p);
  argaddr(1, &rusage_p);
  return kwait2(p, rusage_p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}


uint64
sys_getprocs(void)
{
  uint64 dst_uproc;
  struct proc *p;
  struct pstat kproc;
  int count = 0;

  argaddr(0, &dst_uproc);

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state != UNUSED) {
      kproc.pid = p->pid;
      kproc.state = p->state;
      kproc.size = p->sz;
      kproc.priority = p->priority;
      if(p->parent)
        kproc.ppid = p->parent->pid;
      else
        kproc.ppid = 0;
      safestrcpy(kproc.name, p->name, sizeof(kproc.name));
      release(&p->lock);

      if(copyout(myproc()->pagetable,
                 myproc()->sz,
                 dst_uproc + count * sizeof(struct pstat),
                 (char *)&kproc,
                 sizeof(struct pstat)) < 0) {
        return -1;
      }
      count++;
    } else {
      release(&p->lock);
    }
  }
  return count;
}

uint64
sys_getpriority(void)
{
  struct proc *p = myproc();
  int prio;
  acquire(&p->lock);
  prio = p->priority;
  release(&p->lock);
  return prio;
}

uint64
sys_setpriority(void)
{
  int prio;
  struct proc *p = myproc();

  argint(0, &prio);
  if(prio < 0 || prio > 49)
    return -1;

  acquire(&p->lock);
  p->priority = prio;
  release(&p->lock);
  return 0;
}

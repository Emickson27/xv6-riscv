#ifndef _PSTAT_H_
#define _PSTAT_H_

#include "param.h"

// Define procstate so user-space programs (ps, time) know its size and values
enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

struct rusage {
  uint cputime;
};

struct pstat {
  int pid;               // Process ID
  enum procstate state;  // Process state
  uint64 size;           // Size of process memory (bytes)
  int ppid;              // Parent process ID
  char name[16];         // Parent command name
};

#endif // _PSTAT_H_

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void
workload(int n)
{
  volatile int sum = 0;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < 50000; j++) {
      sum += (i * 3) ^ (j * 5);
    }
  }
}

int
main(int argc, char *argv[])
{
  // Reverse the order: create the highest priority child first!
  int priorities[] = { 35, 20, 5 };
  int num_procs = 3;

  printf("=== Priority Scheduler Test Starting ===\n");

  for (int i = 0; i < num_procs; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("fork failed\n");
      exit(1);
    }

    if (pid == 0) {
      setpriority(priorities[i]);
      workload(25000);
      printf("Process [PID %d] with Priority %d FINISHED\n", getpid(), priorities[i]);
      exit(0);
    }
  }

  // Parent waits for all 3 children to finish
  for (int i = 0; i < num_procs; i++) {
    wait(0);
  }

  printf("=== Priority Scheduler Test Complete ===\n");
  exit(0);
}

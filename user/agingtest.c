#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void
burn(int iterations)
{
  volatile int x = 0;
  for (int i = 0; i < iterations; i++) {
    for (int j = 0; j < 50000; j++) {
      x += (i ^ j);
    }
  }
}

int
main(int argc, char *argv[])
{
  printf("=== Starting Aging Test ===\n");

  // Child 1: Lower Priority (5)
  int pid1 = fork();
  if (pid1 == 0) {
    setpriority(5);
    burn(3000);
    printf("LOW Priority Process (PID %d) Finished!\n", getpid());
    exit(0);
  }

  // Child 2: High Priority (35) with a very long job
  int pid2 = fork();
  if (pid2 == 0) {
    setpriority(35);
    burn(12000);
    printf("HIGH Priority Process (PID %d) Finished!\n", getpid());
    exit(0);
  }

  wait(0);
  wait(0);
  printf("=== Aging Test Complete ===\n");
  exit(0);
}

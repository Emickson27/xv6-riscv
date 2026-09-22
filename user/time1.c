#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[]) {
  if(argc < 2){
    fprintf(2, "Usage: time1 ...\n");
    exit(1);
  }

  int start = uptime();

  int proc = fork();//creates a child process
  if(proc < 0){//if fork is less than 0 it leads to fork failed
    fprintf(2, "fork failed\n");
    exit(1);
  }

  //Assemble the arguments to pass to exec()
  if(proc == 0){
    exec(argv[1], &argv[1]);//executes program to pass as a command
    fprintf(2, "time1: exec %s failed\n", argv[1]);
    exit(1);
  } else {

    // Call wait() in the parent to wait until the child finishes
    int status;
    wait(&status);

    // after wait returns; gives elapsed times
    int end = uptime();
    int elapsed = end-start;

    printf("elapsed time: %d ticks\n", elapsed);
    exit(0);
  }
}

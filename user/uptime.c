#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int ticks = uptime();//trigger system call into kernel
  printf("up %d clock ticks\n", ticks);
  exit(0);
}

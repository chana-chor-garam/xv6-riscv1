#include "kernel/types.h"
#include "user/user.h"
// extern int scheduler_logging_enabled;

void
spin()
{
  for (int i = 0; i < 50000000; i++) {
    asm volatile("nop");
    // Add some yield points to allow preemption
    if(i % 10000000 == 0) {
      printf("Child %d progress: %d%%\n", getpid(), (i * 100) / 50000000);
    }
  }
}

int
main(void)
{
    // scheduler_logging_enabled = 1; // Enable logging for this test
    set_scheduler_logging(1); // Enable logging for this test
  printf("Starting scheduler test...\n");

  for(int i = 0; i < 3; i++) {
    int pid = fork();
    if(pid == 0) { // Child process
      printf("Child %d starting CPU-intensive task.\n", getpid());
      spin();
      printf("Child %d finished.\n", getpid());
      exit(0);
    }
  }

  // Parent waits for all children
  for(int i = 0; i < 3; i++) {
    wait(0);
  }

  printf("Scheduler test finished.\n");
//   scheduler_logging_enabled = 0; // Enable logging for this test
set_scheduler_logging(0); // Disable logging after the test
  exit(0);
}
#include "kernel/types.h"
#include "user/user.h"

void cpu_intensive_task(int duration) {
    int start_time = uptime();
    while (uptime() - start_time < duration) {
        // CPU intensive work
        for (int i = 0; i < 1000; i++) {
            // Busy wait
        }
    }
}

void busy_delay(int ticks) {
    int start = uptime();
    while (uptime() - start < ticks) {
        // Busy wait for delay
    }
}

int main() {
    printf("=== Scheduler Test ===\n");
    
    int pids[3];
    int start_time = uptime();
    
    // Create 3 processes
    for (int i = 0; i < 3; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            // Child process
            int child_start = uptime();
            printf("Child %d starting at time %d\n", getpid(), child_start);
            
            cpu_intensive_task(100); // Run for ~100 ticks
            
            int child_end = uptime();
            printf("Child %d finished at time %d (runtime: %d ticks)\n", 
                   getpid(), child_end, child_end - child_start);
            exit(0);
        }
        busy_delay(5); // Small delay between process creation
    }
    
    // Wait for all children
    for (int i = 0; i < 3; i++) {
        int status;
        wait(&status);
    }
    
    int total_time = uptime() - start_time;
    printf("Total test time: %d ticks\n", total_time);
    printf("Average time per process: %d ticks\n", total_time / 3);
    
    exit(0);
}
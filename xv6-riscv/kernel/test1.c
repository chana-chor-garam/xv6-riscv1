#include "kernel/types.h"
#include "user/user.h"

int main() {
    printf("Test1 started\n");
    for(int i = 0; i < 1000000; i++) {
        // CPU intensive work
        if(i % 100000 == 0) {
            printf("Test1: %d\n", i);
        }
    }
    printf("Test1 finished\n");
    exit(0);
}
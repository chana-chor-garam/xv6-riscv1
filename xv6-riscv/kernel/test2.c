#include "kernel/types.h"
#include "user/user.h"

int main() {
    printf("Test2 started\n");
    for(int i = 0; i < 1000000; i++) {
        if(i % 100000 == 0) {
            printf("Test2: %d\n", i);
        }
    }
    printf("Test2 finished\n");
    exit(0);
}
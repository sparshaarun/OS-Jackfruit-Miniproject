#include <stdio.h>
#include <unistd.h>

int main() {
    for (int i = 0; i < 20; i++) {
        printf("io_hog iteration=%d\n", i);
        fflush(stdout);
        usleep(200000);
    }
    return 0;
}

#include <stdio.h>
#include <stdint.h>
#define N 20000000
static unsigned char flags[N+1];
int main(void) {
    int64_t count = 0;
    for (int64_t i = 2; i <= N; i++) {
        if (flags[i] == 0) { count++; for (int64_t j = i*2; j <= N; j += i) flags[j] = 1; }
    }
    printf("%ld\n", count); return 0;
}

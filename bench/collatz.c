#include <stdio.h>
#include <stdint.h>
int main(void) {
    int64_t best = 0, best_n = 0;
    for (int64_t n = 1; n < 3000000; n++) {
        int64_t x = n, steps = 0;
        while (x != 1) { if (x % 2 == 0) x = x / 2; else x = 3*x+1; steps++; }
        if (steps > best) { best = steps; best_n = n; }
    }
    printf("%ld %ld\n", best_n, best); return 0;
}

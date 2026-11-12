#include <stdio.h>
#include <stdint.h>
#define N 300
static double a[N*N], b[N*N], c[N*N];
int main(void) {
    for (int64_t i = 0; i < N; i++) for (int64_t j = 0; j < N; j++) { a[i*N+j] = (double)(i+j)*0.5; b[i*N+j] = (double)(i-j)*0.25; }
    for (int64_t i = 0; i < N; i++) for (int64_t j = 0; j < N; j++) { double s = 0.0; for (int64_t k = 0; k < N; k++) s += a[i*N+k]*b[k*N+j]; c[i*N+j] = s; }
    printf("%.6f %.6f\n", c[N*17+33], c[N*N-1]); return 0;
}

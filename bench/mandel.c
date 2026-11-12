#include <stdio.h>
#include <stdint.h>
int main(void) {
    int64_t w = 600, h = 400, total = 0;
    for (int64_t py = 0; py < h; py++) for (int64_t px = 0; px < w; px++) {
        double cx = (double)px / (double)w * 3.5 - 2.5, cy = (double)py / (double)h * 2.0 - 1.0;
        double x = 0, y = 0; int64_t it = 0;
        while (it < 200 && x*x + y*y <= 4.0) { double t = x*x - y*y + cx; y = 2.0*x*y + cy; x = t; it++; }
        total += it;
    }
    printf("%ld\n", total); return 0;
}

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "grana.c"
int main(int argc, char **argv)
{
    static int32_t buf[512];
    int k, i, n = 0, jumps = 0, lastjump = -1, sz = 0;
    for (k = 1; k + 1 < argc; k += 2) par[atoi(argv[k])] = (uint8_t)atoi(argv[k + 1]);
    engine_update();
    printf("slice %u frames (22050) = %.1f ms, prob %u\n", eng.st_len, eng.st_len / 22.05, eng.st_prob);
    double prev = 0; long t = 0;
    for (k = 0; k < 2000; k++) {
        for (i = 0; i < 256; i++) { int32_t v = (int32_t)((t % 40000) - 20000); buf[2*i] = buf[2*i+1] = v; t++; }
        mod_block(buf, 256);
        for (i = 0; i < 256; i++) {
            double o = buf[2*i];
            if (k > 10 && o < prev - 400 && prev - o < 30000) { jumps++; if (lastjump >= 0) sz += n - lastjump; lastjump = n; }
            prev = o; n++;
        }
    }
    printf("jumps back %d, mean gap %.1f ms\n", jumps, jumps > 1 ? sz / (double)(jumps - 1) / 44.1 : 0);
    return 0;
}

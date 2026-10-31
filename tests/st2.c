#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "grana.c"
int main(int argc, char **argv)
{
    static int32_t buf[512];
    int k, i, hist[12] = {0}, other = 0;
    for (k = 1; k + 1 < argc; k += 2) par[atoi(argv[k])] = (uint8_t)atoi(argv[k + 1]);
    engine_update();
    long t = 0, L2 = 2 * eng.st_len;
    printf("slice %.1f ms  ", eng.st_len / 22.05);
    for (k = 0; k < 3000; k++) {
        for (i = 0; i < 256; i++) { buf[2*i] = buf[2*i+1] = (int32_t)(t % 30000); t++; }
        mod_block(buf, 256);
        for (i = 0; i < 256; i++) {
            long tin = (t - 256 + i) % 30000, d = tin - buf[2*i];
            if (k < 20 || tin < 20000) continue;
            long m = (d + L2 / 2) / L2;
            if (labs(d - m * L2) <= 2 && m >= 0 && m < 12) hist[m]++; else other++;
        }
    }
    for (i = 0; i < 10; i++) printf("%d:%d ", i, hist[i]);
    printf(" fades/other:%d\n", other);
    return 0;
}

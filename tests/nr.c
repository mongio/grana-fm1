#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "grana.c"
int main(int argc, char **argv)
{
    int k, i, hist[20] = {0}, other = 0, tot = 0;
    for (k = 1; k + 1 < argc; k += 2) par[atoi(argv[k])] = (uint8_t)atoi(argv[k + 1]);
    metal_init(); reverb_init(); engine_update();
    long t = 0, L2 = 2L * eng.n_len;
    printf("modo %d fetta %.1f ms rip %d  ", eng.n_mode, eng.n_len / 22.05, eng.n_reps);
    for (k = 0; k < 400000; k += 2) {
        int32_t l0 = (int32_t)(t % 30000) - 15000, l1 = (int32_t)((t + 1) % 30000) - 15000, yl, yr, dk;
        fx_frame((l0 + l1) >> 1, (l0 + l1) >> 1, &yl, &yr, &dk);
        long o = ((l0 * dk) >> 15) + yl, tin = t % 30000 - 15000, d = tin - o;
        if (t > 44100 && t % 30000 > 8000 && t % 30000 < 22000) {
            long m = (d + L2 / 2) / L2; tot++;
            if (labs(d - m * L2) <= 40 && m >= 0 && m < 20) hist[m]++; else other++;
        }
        t += 2;
    }
    for (i = 0; i < 8; i++) printf("%d:%d ", i, hist[i]);
    printf(" altro:%d\n", other);
    return 0;
}

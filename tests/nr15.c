#include <stdio.h>
#include <stdlib.h>
#include "grana.c"
int main(int argc, char **argv)
{
    int k, hist[20] = {0}, was = 0;
    for (k = 1; k + 1 < argc; k += 2) par[atoi(argv[k])] = (uint8_t)atoi(argv[k + 1]);
    metal_init(); reverb_init(); engine_update();
    for (k = 0; k < 4000000; k++) {
        int32_t yl, yr, dk, x = (int32_t)(xrand() & 0x3FFF) - 8192;
        fx_frame(x, x, &yl, &yr, &dk);
        if (was && !nr_on) hist[nr_rep > 19 ? 19 : nr_rep]++;
        was = nr_on;
    }
    printf("caso %d rip max %d var %d: ", eng.n_caso, eng.n_reps, eng.n_var);
    for (k = 1; k <= 10; k++) printf("%d:%d ", k, hist[k]);
    printf("\n");
    return 0;
}

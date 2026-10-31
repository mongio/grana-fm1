/* measures GRANA's filter and MODULAZIONE on sines: the same grana.c, built natively */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grana.c"
static double run(int which, double f, double secs, double *mn, double *mx)
{
    static int32_t buf[512];
    int N = (int)(secs * 44100), done = 0, warm = 22050, win = 2205;
    double ph = 0, ein = 0, eout = 0, wi = 0, wo = 0; int wc = 0;
    *mn = 1e9; *mx = 0;
    while (done < N + warm) {
        int n = 256, i;
        for (i = 0; i < n; i++) { int32_t v = (int32_t)(8000 * sin(ph)); ph += 2 * M_PI * f / 44100; buf[2*i] = buf[2*i+1] = v; }
        double bi[256]; for (i = 0; i < n; i++) bi[i] = buf[2*i];
        if (which == 0) filter_block(buf, n); else mod_block(buf, n);
        for (i = 0; i < n; i++) if (done + i >= warm) {
            double o = buf[2*i]; ein += bi[i]*bi[i]; eout += o*o; wi += bi[i]*bi[i]; wo += o*o;
            if (++wc == win) { double g = sqrt(wo / wi); if (g < *mn) *mn = g; if (g > *mx) *mx = g; wc = 0; wi = wo = 0; }
        }
        done += n;
    }
    return sqrt(eout / ein);
}
static double db(double g) { return 20 * log10(g + 1e-9); }
int main(int argc, char **argv)
{
    int which = atoi(argv[1]), k;
    for (k = 2; k + 1 < argc; k += 2) par[atoi(argv[k])] = (uint8_t)atoi(argv[k + 1]);
    engine_update();
    static const double F[] = {50, 100, 200, 400, 800, 1600, 3200, 6400, 12800};
    for (k = 0; k < 9; k++) {
        double mn, mx, g = run(which, F[k], which ? 4.0 : 0.5, &mn, &mx);
        printf("%6.0f Hz  %6.1f dB   (min %6.1f  max %6.1f)\n", F[k], db(g), db(mn), db(mx));
    }
    return 0;
}

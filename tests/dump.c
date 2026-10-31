/* GRANA 1.4: a sine through mod_block (mode 0) or fx_frame (mode 1), raw int16 L to stdout */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "grana.c"
int main(int argc, char **argv)
{
    static int32_t buf[512];
    int which = atoi(argv[1]), k, i; double f = atof(argv[2]), secs = atof(argv[3]), ph = 0;
    for (k = 4; k + 1 < argc; k += 2) par[atoi(argv[k])] = (uint8_t)atoi(argv[k + 1]);
    metal_init(); reverb_init(); engine_update();
    for (k = 0; k < (int)(secs * 44100 / 256); k++) {
        for (i = 0; i < 256; i++) { int32_t v = f > 0 ? (int32_t)(8000 * sin(ph)) : (int32_t)((xrand_main() & 0x3FFF) - 8192) / 2; ph += 2 * M_PI * f / 44100; buf[2*i] = buf[2*i+1] = v; }
        if (which == 0) mod_block(buf, 256);
        else for (i = 0; i < 256; i += 2) { int32_t yl, yr, dk, l0 = buf[2*i], l1 = buf[2*i+2];
            fx_frame((l0 + l1) >> 1, (l0 + l1) >> 1, &yl, &yr, &dk);
            buf[2*i] = ((l0 * dk) >> 15) + yl; buf[2*i+2] = ((l1 * dk) >> 15) + yl; }
        for (i = 0; i < 256; i++) { int16_t o = (int16_t)(buf[2*i] > 32767 ? 32767 : buf[2*i] < -32767 ? -32767 : buf[2*i]); fwrite(&o, 2, 1, stdout); }
    }
    return 0;
}

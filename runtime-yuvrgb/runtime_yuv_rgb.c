#include "conversion.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int yuv_rgb_runtime(const char *yuv_file, uint32_t h, uint32_t w)
{
    char rgb_out[512];
    snprintf(rgb_out, sizeof(rgb_out), "%s_out.rgb", yuv_file);

    FILE *in  = fopen(yuv_file, "rb");
    if (!in) { perror("input"); return 1; }
    FILE *out = fopen(rgb_out, "wb");
    if (!out) { perror("output"); fclose(in); return 1; }

    size_t ysz = w * h * 2, rsz = w * h * 3;
    uint8_t *yuv = malloc(ysz), *rgb = malloc(rsz);
    if (!yuv || !rgb) { perror("malloc"); fclose(in); fclose(out); return 1; }

    fread(yuv, 1, ysz, in);
    int ret = convert_fpga(yuv, rgb, w, h);   /* adresele FPGA sunt hard‑codate */

    if (!ret) fwrite(rgb, 1, rsz, out);

    free(yuv); free(rgb); fclose(in); fclose(out);
    return ret;
}

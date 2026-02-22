/*
 * femtojpeg.h — Ultra-minimal baseline JPEG decoder
 *
 * Decodes baseline (SOF0) JPEG to RGB565 via row callbacks.
 * Supports: grayscale, YCbCr 4:4:4, 4:2:2, 4:2:0 subsampling.
 * Does not support: progressive, arithmetic coding, multi-scan.
 *
 * ~1.3 KB static context. No external dependencies. No dynamic allocation
 * except a row buffer sized to output width.
 * Supports 1/4 and 1/8 downscaling for large images.
 *
 * MIT License — see LICENSE file.
 */
#ifndef FEMTOJPEG_H
#define FEMTOJPEG_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t width;
    uint16_t height;
} fjpeg_info_t;

/* Row callback: y = row (0=top), w = width, rgb565 = pixel data. */
typedef void (*fjpeg_row_cb)(int y, int w, const uint16_t *rgb565, void *user);

/* Get image dimensions without decoding. Returns 0 on success. */
int fjpeg_info(const void *data, size_t len, fjpeg_info_t *info);

/* Decode JPEG to RGB565. Calls cb for each row. Returns 0 on success.
 * scale: 1 = full, 4 = quarter, 8 = eighth resolution. */
int fjpeg_decode(const void *data, size_t len, int scale,
                 fjpeg_row_cb cb, void *user);

#endif /* FEMTOJPEG_H */

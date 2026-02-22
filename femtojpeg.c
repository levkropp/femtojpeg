/*
 * femtojpeg.c — Ultra-minimal baseline JPEG decoder
 *
 * Decodes baseline sequential JPEG (SOF0) images to RGB565.
 * Winograd IDCT with 8-bit fixed-point integer math.
 * In-memory input, row-by-row output via callback.
 *
 * Inspired by picojpeg (public domain, Rich Geldreich).
 * Written from scratch for the Survival Workstation project.
 */

#include "femtojpeg.h"
#include <string.h>
#include <stdlib.h>

/*--- Types ---*/

typedef struct {
    uint16_t min_code[16];
    uint16_t max_code[16];
    uint8_t  val_ptr[16];
} huff_table_t;

typedef struct {
    /* Input */
    const uint8_t *data;
    size_t len;
    size_t pos;

    /* Bit reader */
    uint32_t bits;
    int nbits;

    /* Image */
    uint16_t width, height;
    uint8_t ncomp;
    uint8_t hsamp[3], vsamp[3];  /* sampling factors */
    uint8_t comp_qtab[3];        /* quantization table index per component */
    uint8_t comp_dc[3];          /* DC Huffman table index */
    uint8_t comp_ac[3];          /* AC Huffman table index */
    int16_t last_dc[3];          /* previous DC value per component */

    /* MCU geometry */
    uint8_t mcu_w, mcu_h;       /* pixels: 8 or 16 */
    uint16_t mcus_x, mcus_y;

    /* Quantization tables (2), pre-scaled for Winograd IDCT */
    int16_t qtab[2][64];

    /* Huffman tables: 0-1 = DC, 2-3 = AC */
    huff_table_t huff[4];
    uint8_t huff_val[4][256];
    uint8_t huff_nval[4];

    /* Restart interval */
    uint16_t restart_interval;
    uint16_t restarts_left;
    uint8_t  next_restart;

    /* Work buffer */
    int16_t block[64];
} fjctx_t;

/*--- Zigzag order ---*/

static const uint8_t zag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/*--- Winograd IDCT scale factors ---*/

static const uint8_t wquant[64] = {
    128, 178, 178, 167, 246, 167, 151, 232,
    232, 151, 128, 209, 219, 209, 128, 101,
    178, 197, 197, 178, 101,  69, 139, 167,
    177, 167, 139,  69,  35,  96, 131, 151,
    151, 131,  96,  35,  49,  91, 118, 128,
    118,  91,  49,  46,  81, 101, 101,  81,
     46,  42,  69,  79,  69,  42,  35,  54,
     54,  35,  28,  37,  28,  19,  19,  10,
};

/*--- Byte/bit reading ---*/

static uint8_t read_u8(fjctx_t *c)
{
    if (c->pos < c->len) return c->data[c->pos++];
    return 0;
}

static uint16_t read_u16(fjctx_t *c)
{
    uint16_t hi = read_u8(c);
    return (hi << 8) | read_u8(c);
}

/* Get next byte from entropy-coded data, handling 0xFF byte stuffing */
static uint8_t next_byte(fjctx_t *c)
{
    uint8_t b = read_u8(c);
    if (b == 0xFF) {
        uint8_t marker = read_u8(c);
        if (marker != 0) {
            /* Unexpected marker in entropy data — push back for restart handling */
            c->pos -= 2;
            return 0;
        }
    }
    return b;
}

static void fill_bits(fjctx_t *c)
{
    while (c->nbits <= 24) {
        c->bits |= (uint32_t)next_byte(c) << (24 - c->nbits);
        c->nbits += 8;
    }
}

static uint16_t get_bits(fjctx_t *c, int n)
{
    if (n == 0) return 0;
    fill_bits(c);
    uint16_t val = (uint16_t)(c->bits >> (32 - n));
    c->bits <<= n;
    c->nbits -= n;
    return val;
}

static uint8_t get_bit(fjctx_t *c)
{
    fill_bits(c);
    uint8_t val = (c->bits >> 31) & 1;
    c->bits <<= 1;
    c->nbits -= 1;
    return val;
}

/*--- Huffman ---*/

static void huff_build(const uint8_t *counts, huff_table_t *ht)
{
    uint16_t code = 0;
    uint8_t j = 0;
    for (int i = 0; i < 16; i++) {
        if (counts[i] == 0) {
            ht->min_code[i] = 0;
            ht->max_code[i] = 0xFFFF;
            ht->val_ptr[i] = 0;
        } else {
            ht->min_code[i] = code;
            ht->max_code[i] = code + counts[i] - 1;
            ht->val_ptr[i] = j;
            j += counts[i];
            code += counts[i];
        }
        code <<= 1;
    }
}

static uint8_t huff_decode(fjctx_t *c, int table)
{
    huff_table_t *ht = &c->huff[table];
    uint16_t code = get_bit(c);
    for (int i = 0; i < 16; i++) {
        if (code <= ht->max_code[i] && ht->max_code[i] != 0xFFFF) {
            uint8_t j = ht->val_ptr[i] + (uint8_t)(code - ht->min_code[i]);
            return c->huff_val[table][j];
        }
        code = (code << 1) | get_bit(c);
    }
    return 0;
}

/* Sign-extend a Huffman-decoded value */
static int16_t huff_extend(uint16_t val, uint8_t bits)
{
    if (bits == 0) return 0;
    if (val < (1u << (bits - 1)))
        return (int16_t)val - (int16_t)((1u << bits) - 1);
    return (int16_t)val;
}

/*--- Marker parsing ---*/

static int parse_dqt(fjctx_t *c)
{
    uint16_t left = read_u16(c) - 2;
    while (left > 0) {
        uint8_t info = read_u8(c);
        uint8_t prec = info >> 4;
        uint8_t id = info & 0x0F;
        if (id > 1) return -1;
        for (int i = 0; i < 64; i++) {
            int16_t val = read_u8(c);
            if (prec) val = (val << 8) | read_u8(c);
            c->qtab[id][i] = val;
        }
        /* Pre-multiply by Winograd scale factors */
        for (int i = 0; i < 64; i++) {
            long x = (long)c->qtab[id][i] * wquant[i];
            c->qtab[id][i] = (int16_t)((x + (1 << 2)) >> 3);
        }
        left -= 65 + (prec ? 64 : 0);
    }
    return 0;
}

static int parse_dht(fjctx_t *c)
{
    uint16_t left = read_u16(c) - 2;
    while (left > 0) {
        uint8_t info = read_u8(c);
        uint8_t cls = (info >> 4) & 1;  /* 0=DC, 1=AC */
        uint8_t id = info & 1;
        int table = cls * 2 + id;

        uint8_t counts[16];
        uint16_t total = 0;
        for (int i = 0; i < 16; i++) {
            counts[i] = read_u8(c);
            total += counts[i];
        }
        for (int i = 0; i < total && i < 256; i++)
            c->huff_val[table][i] = read_u8(c);
        c->huff_nval[table] = (uint8_t)total;

        huff_build(counts, &c->huff[table]);
        left -= 17 + total;
    }
    return 0;
}

static int parse_sof(fjctx_t *c)
{
    read_u16(c); /* length */
    if (read_u8(c) != 8) return -1; /* precision must be 8 */
    c->height = read_u16(c);
    c->width = read_u16(c);
    c->ncomp = read_u8(c);
    if (c->ncomp != 1 && c->ncomp != 3) return -1;

    for (int i = 0; i < c->ncomp; i++) {
        read_u8(c); /* component ID — ignored, assume sequential */
        uint8_t samp = read_u8(c);
        c->hsamp[i] = samp >> 4;
        c->vsamp[i] = samp & 0x0F;
        c->comp_qtab[i] = read_u8(c);
    }

    /* MCU size */
    if (c->ncomp == 1) {
        c->mcu_w = 8; c->mcu_h = 8;
    } else {
        c->mcu_w = c->hsamp[0] * 8;
        c->mcu_h = c->vsamp[0] * 8;
    }
    c->mcus_x = (c->width + c->mcu_w - 1) / c->mcu_w;
    c->mcus_y = (c->height + c->mcu_h - 1) / c->mcu_h;
    return 0;
}

static int parse_sos(fjctx_t *c)
{
    uint16_t left = read_u16(c) - 2;
    uint8_t ns = read_u8(c);
    left--;
    for (int i = 0; i < ns; i++) {
        uint8_t id = read_u8(c); (void)id;
        uint8_t tab = read_u8(c);
        c->comp_dc[i] = tab >> 4;
        c->comp_ac[i] = (tab & 0x0F);
        left -= 2;
    }
    /* Skip spectral selection and successive approximation */
    while (left > 0) { read_u8(c); left--; }
    return 0;
}

static int parse_dri(fjctx_t *c)
{
    read_u16(c); /* length = 4 */
    c->restart_interval = read_u16(c);
    return 0;
}

static void skip_marker(fjctx_t *c)
{
    uint16_t len = read_u16(c);
    if (len >= 2) c->pos += len - 2;
}

static int parse_markers(fjctx_t *c)
{
    /* Find SOI */
    if (read_u8(c) != 0xFF || read_u8(c) != 0xD8) return -1;

    while (c->pos < c->len) {
        uint8_t b = read_u8(c);
        if (b != 0xFF) continue;
        do { b = read_u8(c); } while (b == 0xFF);
        if (b == 0) continue;

        switch (b) {
            case 0xC0: if (parse_sof(c)) return -1; break;
            case 0xC4: if (parse_dht(c)) return -1; break;
            case 0xDB: if (parse_dqt(c)) return -1; break;
            case 0xDD: if (parse_dri(c)) return -1; break;
            case 0xDA: if (parse_sos(c)) return -1; return 0; /* SOS = start of data */
            case 0xD9: return -1; /* EOI before SOS */
            case 0xC2: return -1; /* progressive not supported */
            default: skip_marker(c); break;
        }
    }
    return -1;
}

/*--- Winograd IDCT ---*/

#define IDCT_SCALE 7
#define IDCT_ROUND (1 << (IDCT_SCALE - 1))
#define DESCALE(x) (((x) + IDCT_ROUND) >> IDCT_SCALE)

/* Winograd multiply helpers — 5 constants */
static int16_t imul_362(int16_t w) { return (int16_t)(((long)w * 362 + 128) >> 8); }
static int16_t imul_669(int16_t w) { return (int16_t)(((long)w * 669 + 128) >> 8); }
static int16_t imul_277(int16_t w) { return (int16_t)(((long)w * 277 + 128) >> 8); }
static int16_t imul_196(int16_t w) { return (int16_t)(((long)w * 196 + 128) >> 8); }

static uint8_t clamp8(int16_t x)
{
    if (x < 0) return 0;
    if (x > 255) return 255;
    return (uint8_t)x;
}

static void idct_rows(int16_t *b)
{
    for (int i = 0; i < 8; i++, b += 8) {
        if (!(b[1] | b[2] | b[3] | b[4] | b[5] | b[6] | b[7])) {
            b[1]=b[2]=b[3]=b[4]=b[5]=b[6]=b[7]=b[0];
            continue;
        }
        int16_t s4=b[5], s7=b[3], x4=s4-s7, x7=s4+s7;
        int16_t s5=b[1], s6=b[7], x5=s5+s6, x6=s5-s6;
        int16_t t1=imul_196(x4-x6);
        int16_t st26=imul_277(x6)-t1;
        int16_t x24=t1-imul_669(x4);
        int16_t x15=x5-x7, x17=x5+x7;
        int16_t t2=st26-x17;
        int16_t t3=imul_362(x15)-t2;
        int16_t x44=t3+x24;
        int16_t s0=b[0], s1=b[4];
        int16_t x30=s0+s1, x31=s0-s1;
        int16_t s2=b[2], s3=b[6];
        int16_t x12=s2-s3, x13=s2+s3;
        int16_t x32=imul_362(x12)-x13;
        int16_t x40=x30+x13, x43=x30-x13;
        int16_t x41=x31+x32, x42=x31-x32;
        b[0]=x40+x17; b[1]=x41+t2; b[2]=x42+t3; b[3]=x43-x44;
        b[4]=x43+x44; b[5]=x42-t3; b[6]=x41-t2; b[7]=x40-x17;
    }
}

static void idct_cols(int16_t *b, uint8_t *out)
{
    for (int i = 0; i < 8; i++, b++) {
        if (!(b[8] | b[16] | b[24] | b[32] | b[40] | b[48] | b[56])) {
            uint8_t v = clamp8(DESCALE(b[0]) + 128);
            for (int j = 0; j < 8; j++) out[j * 8 + i] = v;
            continue;
        }
        int16_t s4=b[40], s7=b[24], x4=s4-s7, x7=s4+s7;
        int16_t s5=b[8],  s6=b[56], x5=s5+s6, x6=s5-s6;
        int16_t t1=imul_196(x4-x6);
        int16_t st26=imul_277(x6)-t1;
        int16_t x24=t1-imul_669(x4);
        int16_t x15=x5-x7, x17=x5+x7;
        int16_t t2=st26-x17;
        int16_t t3=imul_362(x15)-t2;
        int16_t x44=t3+x24;
        int16_t s0=b[0], s1=b[32];
        int16_t x30=s0+s1, x31=s0-s1;
        int16_t s2=b[16], s3=b[48];
        int16_t x12=s2-s3, x13=s2+s3;
        int16_t x32=imul_362(x12)-x13;
        int16_t x40=x30+x13, x43=x30-x13;
        int16_t x41=x31+x32, x42=x31-x32;
        out[0*8+i]=clamp8(DESCALE(x40+x17)+128);
        out[1*8+i]=clamp8(DESCALE(x41+t2)+128);
        out[2*8+i]=clamp8(DESCALE(x42+t3)+128);
        out[3*8+i]=clamp8(DESCALE(x43-x44)+128);
        out[4*8+i]=clamp8(DESCALE(x43+x44)+128);
        out[5*8+i]=clamp8(DESCALE(x42-t3)+128);
        out[6*8+i]=clamp8(DESCALE(x41-t2)+128);
        out[7*8+i]=clamp8(DESCALE(x40-x17)+128);
    }
}

/*--- Block decoding ---*/

static int decode_block(fjctx_t *c, int comp, uint8_t *pixels)
{
    int16_t *blk = c->block;
    memset(blk, 0, sizeof(c->block));

    int qtab = c->comp_qtab[comp];
    const int16_t *q = c->qtab[qtab];

    /* DC coefficient */
    int dc_tab = c->comp_dc[comp];
    uint8_t s = huff_decode(c, dc_tab);
    uint8_t nbits = s & 0x0F;
    int16_t dc = huff_extend(get_bits(c, nbits), nbits);
    dc += c->last_dc[comp];
    c->last_dc[comp] = dc;
    blk[0] = dc * q[0];

    /* AC coefficients */
    int ac_tab = c->comp_ac[comp] + 2;
    for (int k = 1; k < 64; k++) {
        s = huff_decode(c, ac_tab);
        uint8_t run = s >> 4;
        uint8_t size = s & 0x0F;
        if (size == 0) {
            if (run == 15) { k += 15; continue; } /* ZRL: skip 16 zeros */
            break; /* EOB */
        }
        k += run;
        if (k >= 64) return -1;
        int16_t ac = huff_extend(get_bits(c, size), size);
        blk[zag[k]] = ac * q[k];
    }

    /* IDCT */
    idct_rows(blk);
    idct_cols(blk, pixels);
    return 0;
}

/*--- Restart processing ---*/

static void process_restart(fjctx_t *c)
{
    /* Scan for restart marker */
    c->nbits = 0;
    c->bits = 0;
    while (c->pos < c->len - 1) {
        if (c->data[c->pos] == 0xFF && c->data[c->pos + 1] >= 0xD0 &&
            c->data[c->pos + 1] <= 0xD7) {
            c->pos += 2;
            break;
        }
        c->pos++;
    }
    c->last_dc[0] = c->last_dc[1] = c->last_dc[2] = 0;
    c->restarts_left = c->restart_interval;
    c->next_restart = (c->next_restart + 1) & 7;
}

/*--- Main decode ---*/

int fjpeg_info(const void *data, size_t len, fjpeg_info_t *info)
{
    const uint8_t *p = data;
    const uint8_t *end = p + len;
    if (len < 2 || p[0] != 0xFF || p[1] != 0xD8) return -1;
    p += 2;
    while (p + 4 <= end) {
        if (*p != 0xFF) { p++; continue; }
        uint8_t marker = p[1];
        if (marker == 0xC0) {
            if (p + 9 > end) return -1;
            info->height = (p[5] << 8) | p[6];
            info->width  = (p[7] << 8) | p[8];
            return 0;
        }
        if (marker == 0xD9) break;
        uint16_t mlen = (p[2] << 8) | p[3];
        p += 2 + mlen;
    }
    return -1;
}

int fjpeg_decode(const void *data, size_t len, fjpeg_row_cb cb, void *user)
{
    fjctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.data = data;
    ctx.len = len;

    if (parse_markers(&ctx) != 0) return -1;
    if (ctx.width == 0 || ctx.height == 0) return -1;

    /* Init restart state */
    if (ctx.restart_interval) {
        ctx.restarts_left = ctx.restart_interval;
        ctx.next_restart = 0;
    }

    /* Allocate row buffer: mcu_h rows of width pixels (RGB565) */
    int row_stride = ctx.width;
    uint16_t *row_buf = malloc((size_t)row_stride * ctx.mcu_h * sizeof(uint16_t));
    if (!row_buf) return -1;

    /* Chroma subsampling shifts */
    int h_shift = (ctx.ncomp > 1 && ctx.hsamp[0] > 1) ? 1 : 0;
    int v_shift = (ctx.ncomp > 1 && ctx.vsamp[0] > 1) ? 1 : 0;

    /* Number of 8x8 Y blocks per MCU */
    int ny_h = ctx.ncomp == 1 ? 1 : ctx.hsamp[0];
    int ny_v = ctx.ncomp == 1 ? 1 : ctx.vsamp[0];

    /* Decode MCU by MCU */
    uint8_t y_blocks[4][64];  /* up to 4 Y blocks (for H2V2) */
    uint8_t cb_block[64], cr_block[64];

    for (int mcu_y = 0; mcu_y < ctx.mcus_y; mcu_y++) {
        memset(row_buf, 0, (size_t)row_stride * ctx.mcu_h * sizeof(uint16_t));

        for (int mcu_x = 0; mcu_x < ctx.mcus_x; mcu_x++) {
            /* Restart interval check */
            if (ctx.restart_interval) {
                if (ctx.restarts_left == 0)
                    process_restart(&ctx);
                ctx.restarts_left--;
            }

            /* Decode Y blocks */
            for (int vy = 0; vy < ny_v; vy++) {
                for (int hx = 0; hx < ny_h; hx++) {
                    if (decode_block(&ctx, 0, y_blocks[vy * ny_h + hx]) != 0)
                        goto fail;
                }
            }

            /* Decode Cb, Cr blocks */
            if (ctx.ncomp == 3) {
                if (decode_block(&ctx, 1, cb_block) != 0) goto fail;
                if (decode_block(&ctx, 2, cr_block) != 0) goto fail;
            }

            /* Convert MCU to RGB565 in row_buf */
            int px0 = mcu_x * ctx.mcu_w;
            for (int py = 0; py < ctx.mcu_h; py++) {
                int img_y = mcu_y * ctx.mcu_h + py;
                if (img_y >= ctx.height) break;

                for (int px = 0; px < ctx.mcu_w; px++) {
                    int img_x = px0 + px;
                    if (img_x >= ctx.width) break;

                    uint8_t Y, Cb_v, Cr_v;

                    if (ctx.ncomp == 1) {
                        Y = y_blocks[0][py * 8 + px];
                        Cb_v = 128; Cr_v = 128;
                    } else {
                        /* Which Y block? */
                        int yb = (py >> 3) * ny_h + (px >> 3);
                        Y = y_blocks[yb][(py & 7) * 8 + (px & 7)];

                        /* Chroma with nearest-neighbor upsampling */
                        int cx = px >> h_shift;
                        int cy = py >> v_shift;
                        Cb_v = cb_block[cy * 8 + cx];
                        Cr_v = cr_block[cy * 8 + cx];
                    }

                    /* YCbCr to RGB (fixed-point) */
                    int cr = (int)Cr_v - 128;
                    int cb_val = (int)Cb_v - 128;
                    int r = (int)Y + ((cr * 359) >> 8);
                    int g = (int)Y - ((cb_val * 88 + cr * 183) >> 8);
                    int b = (int)Y + ((cb_val * 454) >> 8);

                    if (r < 0) r = 0;
                    if (r > 255) r = 255;
                    if (g < 0) g = 0;
                    if (g > 255) g = 255;
                    if (b < 0) b = 0;
                    if (b > 255) b = 255;

                    row_buf[py * row_stride + img_x] =
                        ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                }
            }
        }

        /* Deliver pixel rows */
        for (int py = 0; py < ctx.mcu_h; py++) {
            int img_y = mcu_y * ctx.mcu_h + py;
            if (img_y >= ctx.height) break;
            cb(img_y, ctx.width, row_buf + py * row_stride, user);
        }
    }

    free(row_buf);
    return 0;

fail:
    free(row_buf);
    return -1;
}

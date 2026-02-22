# femtojpeg

Ultra-minimal baseline JPEG decoder for embedded systems.

~800 lines of C. Zero external dependencies. Decodes JPEG images to RGB565 row-by-row via callback. Winograd IDCT with fixed-point integer math. Built-in 1/4 and 1/8 downscaling.

## Features

- Streaming row-by-row output via callback (no full-image buffer needed)
- Direct RGB565 output (native format for most embedded LCD displays)
- Baseline sequential JPEG (SOF0)
- Chroma subsampling: grayscale, 4:4:4 (H1V1), 4:2:2 (H2V1), 4:2:0 (H2V2)
- Winograd IDCT (80 multiplies per 8x8 block vs. 1024 naive)
- 1/4 scale (IDCT + 4x4 averaging) and 1/8 scale (DC-only, no IDCT)
- Restart marker support (DRI)
- ~1.3 KB static context + one row buffer (~7 KB total for 320px H2V2 at 1:1)
- Two-pass decode for H2V2 at 1:1 halves row buffer vs. naive approach
- No external dependencies -- no libc math, no zlib, nothing

## Limitations

- Baseline only (no progressive, no arithmetic coding, no multi-scan)
- No EXIF/JFIF metadata parsing
- No CMYK
- Nearest-neighbor chroma upsampling (not bilinear)

## API

```c
#include "femtojpeg.h"

/* Get image dimensions without decoding */
fjpeg_info_t info;
fjpeg_info(jpeg_data, jpeg_len, &info);
printf("%ux%u\n", info.width, info.height);

/* Decode with row callback (scale: 1, 4, or 8) */
void my_row(int y, int w, const uint16_t *rgb565, void *user) {
    /* blit rgb565 to display at row y */
}
fjpeg_decode(jpeg_data, jpeg_len, 1, my_row, NULL);   /* full size */
fjpeg_decode(jpeg_data, jpeg_len, 4, my_row, NULL);   /* 1/4 size */
fjpeg_decode(jpeg_data, jpeg_len, 8, my_row, NULL);   /* 1/8 size */
```

Both functions take the entire JPEG file in memory. `fjpeg_decode` calls the callback once per row (y=0 is the top row). The `rgb565` buffer is reused between rows -- consume it immediately. The `scale` parameter controls output resolution: 1 for full, 4 for quarter, 8 for eighth.

## Building

Just compile `femtojpeg.c` and add the directory to your include path. No dependencies to link.

### ESP-IDF

```cmake
idf_component_register(
    SRCS "main.c" "path/to/femtojpeg.c"
    INCLUDE_DIRS "." "path/to/femtojpeg"
)
```

### CMake

```cmake
add_executable(myapp main.c femtojpeg.c)
```

## Comparison

| Library | Lines | License | Streaming | RGB565 | Progressive | Scaling | Dependencies | RAM |
|---------|------:|---------|:---------:|:------:|:-----------:|:-------:|:------------:|----:|
| **femtojpeg** | **~800** | **MIT** | **yes** | **yes** | no | **1/4/8** | **none** | **~7 KB** |
| picojpeg | ~2,500 | PD/MIT | MCU-level | no | no | 1/8 reduce | none | ~2.3 KB |
| TJpgDec | ~1,800 | BSD | yes | yes | no | 1/2/4/8 | none | ~3.5 KB |
| NanoJPEG | ~900 | MIT | no | no | no | no | none | ~512 KB |
| stb_image | ~2,500 | PD/MIT | no | no | yes | no | none | full image |
| esp_jpeg | ~1,800 | Apache-2.0 | yes | yes | no | 1/2/4/8 | ESP-IDF | ~3.5 KB |

femtojpeg prioritizes minimal code size and zero dependencies. RAM usage is ~7 KB for 320px H2V2 at 1:1 (two-pass decode halves the row buffer), ~4 KB at 1/4 scale, and ~3 KB at 1/8 scale. The 1/8 mode is DC-only (no IDCT), making it very fast for generating thumbnails from large images.

### Why not TJpgDec?

TJpgDec is excellent and ships with ESP-IDF. If you're already using ESP-IDF, `esp_jpeg` is the pragmatic choice. femtojpeg exists for projects that want a self-contained decoder with no framework dependencies, or for educational purposes -- every line is written to be readable and explainable.

## License

MIT -- see [LICENSE](LICENSE).

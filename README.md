# femtojpeg

Ultra-minimal baseline JPEG decoder for embedded systems.

644 lines of C. Zero external dependencies. Decodes JPEG images to RGB565 row-by-row via callback. Winograd IDCT with fixed-point integer math.

## Features

- Streaming row-by-row output via callback (no full-image buffer needed)
- Direct RGB565 output (native format for most embedded LCD displays)
- Baseline sequential JPEG (SOF0)
- Chroma subsampling: grayscale, 4:4:4 (H1V1), 4:2:2 (H2V1), 4:2:0 (H2V2)
- Winograd IDCT (80 multiplies per 8x8 block vs. 1024 naive)
- Restart marker support (DRI)
- ~2 KB static context + one row buffer (~13 KB total for 320px wide images)
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

/* Decode with row callback */
void my_row(int y, int w, const uint16_t *rgb565, void *user) {
    /* blit rgb565 to display at row y */
}
fjpeg_decode(jpeg_data, jpeg_len, my_row, NULL);
```

Both functions take the entire JPEG file in memory. `fjpeg_decode` calls the callback once per row (y=0 is the top row). The `rgb565` buffer is reused between rows -- consume it immediately.

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
| **femtojpeg** | **644** | **MIT** | **yes** | **yes** | no | no | **none** | **~13 KB** |
| picojpeg | ~2,500 | PD/MIT | MCU-level | no | no | 1/8 reduce | none | ~2.3 KB |
| TJpgDec | ~1,800 | BSD | yes | yes | no | 1/2/4/8 | none | ~3.5 KB |
| NanoJPEG | ~900 | MIT | no | no | no | no | none | ~512 KB |
| stb_image | ~2,500 | PD/MIT | no | no | yes | no | none | full image |
| esp_jpeg | ~1,800 | Apache-2.0 | yes | yes | no | 1/2/4/8 | ESP-IDF | ~3.5 KB |

femtojpeg prioritizes minimal code size and zero dependencies over features like scaling or progressive support. It uses more RAM than picojpeg or TJpgDec (~13 KB vs ~3 KB) because it buffers a full MCU row of RGB565 output, but this enables clean row-by-row callbacks rather than requiring the caller to assemble MCU blocks.

### Why not TJpgDec?

TJpgDec is excellent and ships with ESP-IDF. If you're already using ESP-IDF, `esp_jpeg` is the pragmatic choice. femtojpeg exists for projects that want a self-contained decoder with no framework dependencies, or for educational purposes -- every line is written to be readable and explainable.

## License

MIT -- see [LICENSE](LICENSE).

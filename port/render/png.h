/* A PNG writer with no library: RGB rows in stored (uncompressed) deflate blocks. From King's Field
 * IV's port/platform/video.c (port_png_write). Pictures from tests stay in out/ or a temp folder. */
#ifndef ELTALE_RENDER_PNG_H
#define ELTALE_RENDER_PNG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `rgba`: width x height pixels, R in the low byte (alpha ignored). Returns 1 on success. */
int render_png_write(const char *path, const uint32_t *rgba, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif

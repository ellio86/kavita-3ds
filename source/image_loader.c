#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#include "stb_image.h"

#include "image_loader.h"
#include "debug_log.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static int next_pow2(int v) {
    if (v <= 0) return 64;
    v--;
    v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
    v |= v >> 8;  v |= v >> 16;
    v++;
    return v < 64 ? 64 : v;
}

/* Morton (Z-order) index within an 8×8 tile.
   Interleaves lower 3 bits of x (even positions) and y (odd positions). */
static inline u32 morton_index(u32 x, u32 y) {
    return (x & 1)        |
           ((y & 1) << 1) |
           ((x & 2) << 1) |
           ((y & 2) << 2) |
           ((x & 4) << 2) |
           ((y & 4) << 3);
}

/* Box-filter downscale.  src is RGBA8, dst must be pre-allocated to
   (dst_w * dst_h * 4) bytes. */
static void box_downscale(const u8* src, int src_w, int src_h,
                            u8* dst, int dst_w, int dst_h) {
    float x_ratio = (float)src_w / dst_w;
    float y_ratio = (float)src_h / dst_h;

    for (int dy = 0; dy < dst_h; dy++) {
        for (int dx = 0; dx < dst_w; dx++) {
            int sx0 = (int)(dx * x_ratio);
            int sy0 = (int)(dy * y_ratio);
            int sx1 = (int)((dx + 1) * x_ratio);
            int sy1 = (int)((dy + 1) * y_ratio);
            if (sx1 > src_w) sx1 = src_w;
            if (sy1 > src_h) sy1 = src_h;

            u32 r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++) {
                    const u8* px = src + (sy * src_w + sx) * 4;
                    r += px[0]; g += px[1]; b += px[2]; a += px[3];
                    n++;
                }
            }
            u8* out = dst + (dy * dst_w + dx) * 4;
            out[0] = n ? (u8)(r / n) : 0;
            out[1] = n ? (u8)(g / n) : 0;
            out[2] = n ? (u8)(b / n) : 0;
            out[3] = n ? (u8)(a / n) : 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Phase 1: CPU work (safe to call from background thread)              */
/* ------------------------------------------------------------------ */
bool image_prepare_from_mem(const u8* data, size_t data_size,
                              PreparedTexture* out) {
    if (!data || !data_size || !out) return false;
    memset(out, 0, sizeof(*out));

    int w, h, channels;
    u8* pixels = stbi_load_from_memory(data, (int)data_size,
                                        &w, &h, &channels, 4);
    if (!pixels) {
        unsigned m0 = data_size > 0 ? data[0] : 0;
        unsigned m1 = data_size > 1 ? data[1] : 0;
        unsigned m2 = data_size > 2 ? data[2] : 0;
        unsigned m3 = data_size > 3 ? data[3] : 0;
        dlog("[img][ERR] stbi_load_from_memory fail bytes=%zu head=%02x%02x%02x%02x",
             data_size, m0, m1, m2, m3);
        return false;
    }

    bool pixels_from_malloc = false;

    /* Downscale if either dimension exceeds GPU-friendly size (POT pad ≤ ~4 MiB). */
    int target_w = w, target_h = h;
    if (target_w > 1024 || target_h > 1024) {
        float scale = 1.0f;
        if (target_w > 1024) scale = 1024.0f / target_w;
        if (target_h > 1024 && (1024.0f / target_h) < scale)
            scale = 1024.0f / target_h;
        target_w = (int)(target_w * scale);
        target_h = (int)(target_h * scale);
        if (target_w < 1) target_w = 1;
        if (target_h < 1) target_h = 1;

        u8* scaled = (u8*)malloc((size_t)(target_w * target_h * 4));
        if (!scaled) {
            dlog("[img][ERR] malloc scaled fail %dx%d", target_w, target_h);
            stbi_image_free(pixels);
            return false;
        }
        box_downscale(pixels, w, h, scaled, target_w, target_h);
        stbi_image_free(pixels);
        pixels = scaled;
        pixels_from_malloc = true;
        w = target_w;
        h = target_h;
    }

    int tex_w = next_pow2(w);
    int tex_h = next_pow2(h);

    /* Allocate linearAlloc'd tiled buffer */
    size_t buf_size = (size_t)(tex_w * tex_h * 4);
    u8* tiled = (u8*)linearAlloc(buf_size);
    if (!tiled) {
        dlog("[img][ERR] linearAlloc tiled fail %dx%d (image %dx%d)", tex_w, tex_h, w, h);
        if (pixels_from_malloc) free(pixels);
        else stbi_image_free(pixels);
        return false;
    }
    memset(tiled, 0, buf_size);

    /* Morton swizzle: 8×8 tiles, RGBA (stb) → ABGR for GPU_RGBA8 */
    int tiles_x = tex_w / 8;
    for (int ty = 0; ty < tex_h; ty += 8) {
        for (int tx = 0; tx < tex_w; tx += 8) {
            u32 tile_idx = (ty / 8) * tiles_x + (tx / 8);
            for (int py = 0; py < 8; py++) {
                for (int px = 0; px < 8; px++) {
                    int src_x = tx + px;
                    int src_y = ty + py;
                    u32 m = morton_index((u32)px, (u32)py);
                    u32 dst_byte = (tile_idx * 64 + m) * 4;

                    if (src_x < w && src_y < h) {
                        const u8* src_px = pixels + (src_y * w + src_x) * 4;
                        /* RGBA (stb) → ABGR (GPU_RGBA8 memory layout) */
                        tiled[dst_byte + 0] = src_px[3]; /* A */
                        tiled[dst_byte + 1] = src_px[2]; /* B */
                        tiled[dst_byte + 2] = src_px[1]; /* G */
                        tiled[dst_byte + 3] = src_px[0]; /* R */
                    }
                    /* else: stays zeroed (transparent black padding) */
                }
            }
        }
    }

    /* Free decoded pixels (may be stbi or malloc depending on scaling) */
    if (pixels_from_malloc) free(pixels);
    else stbi_image_free(pixels);

    out->tiled_buf = tiled;
    out->tex_w     = tex_w;
    out->tex_h     = tex_h;
    out->src_w     = w;
    out->src_h     = h;
    out->valid     = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Phase 2: GPU upload (MUST run on main thread)                        */
/* ------------------------------------------------------------------ */
bool image_upload_prepared(PreparedTexture* prep, LoadedTexture* out) {
    if (!prep || !prep->valid || !prep->tiled_buf || !out) {
        dlog("[img][ERR] upload_prepared bad args prep=%p valid=%d buf=%p out=%p",
             (void*)prep, prep ? (int)prep->valid : -1,
             prep ? (void*)prep->tiled_buf : NULL, (void*)out);
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (!C3D_TexInit(&out->tex, prep->tex_w, prep->tex_h, GPU_RGBA8)) {
        dlog("[img][ERR] C3D_TexInit fail POT %dx%d src %dx%d (GPU_RGBA8)",
             prep->tex_w, prep->tex_h, prep->src_w, prep->src_h);
        return false;
    }
    C3D_TexUpload(&out->tex, prep->tiled_buf);

    linearFree(prep->tiled_buf);
    prep->tiled_buf = NULL;
    prep->valid     = false;

    /* SubTexture: map actual image pixels within POT-padded texture */
    out->subtex.width   = (u16)prep->src_w;
    out->subtex.height  = (u16)prep->src_h;
    out->subtex.left    = 0.0f;
    out->subtex.top     = 1.0f;
    out->subtex.right   = (float)prep->src_w / prep->tex_w;
    out->subtex.bottom  = 1.0f - (float)prep->src_h / prep->tex_h;

    out->image.tex    = &out->tex;
    out->image.subtex = &out->subtex;
    out->src_width    = prep->src_w;
    out->src_height   = prep->src_h;
    out->valid        = true;

    return true;
}

void image_prepared_free(PreparedTexture* prep) {
    if (!prep) return;
    if (prep->tiled_buf) {
        linearFree(prep->tiled_buf);
        prep->tiled_buf = NULL;
    }
    prep->valid = false;
}

/* ------------------------------------------------------------------ */
/* Convenience: full one-shot load (main thread only)                   */
/* ------------------------------------------------------------------ */
bool image_load_from_mem(const u8* data, size_t data_size, LoadedTexture* out) {
    PreparedTexture prep;
    if (!image_prepare_from_mem(data, data_size, &prep)) return false;
    return image_upload_prepared(&prep, out);
}

void image_texture_free(LoadedTexture* tex) {
    if (!tex || !tex->valid) return;
    C3D_TexDelete(&tex->tex);
    tex->valid = false;
}

/* ------------------------------------------------------------------ */
/* Fit helper                                                           */
/* ------------------------------------------------------------------ */
void image_fit_dims(int src_w, int src_h,
                    int box_w, int box_h,
                    float* out_scale_x, float* out_scale_y,
                    float* out_offset_x, float* out_offset_y) {
    if (src_w <= 0 || src_h <= 0) {
        *out_scale_x = *out_scale_y = 1.0f;
        *out_offset_x = *out_offset_y = 0.0f;
        return;
    }
    float sx = (float)box_w / src_w;
    float sy = (float)box_h / src_h;
    float s  = sx < sy ? sx : sy;

    float draw_w = src_w * s;
    float draw_h = src_h * s;

    *out_scale_x  = s;
    *out_scale_y  = s;
    *out_offset_x = (box_w - draw_w) * 0.5f;
    *out_offset_y = (box_h - draw_h) * 0.5f;
}

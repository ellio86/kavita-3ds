#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* A loaded GPU texture with citro2d render info                        */
/* ------------------------------------------------------------------ */
typedef struct {
    C3D_Tex           tex;
    Tex3DS_SubTexture subtex;
    C2D_Image         image;    /* image.tex = &tex, image.subtex = &subtex */
    bool              valid;
    int               src_width;   /* original image dimensions (pre-POT padding) */
    int               src_height;
} LoadedTexture;

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/* Decode JPEG/PNG from raw bytes and upload to GPU.
   NOTE: C3D_TexInit and C3D_TexUpload are called here — this function
   MUST be called from the main thread (the thread that owns the GPU context).

   For the background-thread pattern, call image_prepare_from_mem() on the
   worker thread to do the CPU work, then call image_upload_prepared() on
   the main thread to finish the GPU upload.

   Returns true on success. tex must be freed with image_texture_free(). */
bool image_load_from_mem(const u8* data, size_t data_size, LoadedTexture* out);

/* Free a texture previously loaded with image_load_from_mem. */
void image_texture_free(LoadedTexture* tex);

/* ------------------------------------------------------------------ */
/* Two-phase loading (for background thread safety)                     */
/* ------------------------------------------------------------------ */

/* Intermediate buffer produced by the background thread */
typedef struct {
    u8*  tiled_buf;     /* linearAlloc'd Morton-swizzled pixel data */
    int  tex_w;
    int  tex_h;
    int  src_w;
    int  src_h;
    bool valid;
} PreparedTexture;

/* Phase 1 (worker thread): decode + Morton swizzle into linearAlloc.
   Does NOT touch the GPU. */
bool image_prepare_from_mem(const u8* data, size_t data_size,
                              PreparedTexture* out);

/* Phase 2 (main thread): init C3D_Tex, upload, free tiled_buf.
   Consumes the PreparedTexture (sets out_prep->valid = false). */
bool image_upload_prepared(PreparedTexture* prep, LoadedTexture* out);

void image_prepared_free(PreparedTexture* prep);

/* ------------------------------------------------------------------ */
/* Scale a C2D_Image to fit inside a bounding box while preserving AR  */
/* ------------------------------------------------------------------ */
void image_fit_dims(int src_w, int src_h,
                    int box_w, int box_h,
                    float* out_scale_x, float* out_scale_y,
                    float* out_offset_x, float* out_offset_y);

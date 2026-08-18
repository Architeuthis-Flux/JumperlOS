#include "Images.h"

#include <string.h>

#if JL_USE_COMPRESSED_STARTUP_FRAMES
#include "lib/uzlib/uzlib.h"

// Max uzlib window bits we support for built-in assets.
// Must be >= the --wbits used in generate_images_deflate.py.
#ifndef JL_STARTUP_UZLIB_WBITS_MAX
#define JL_STARTUP_UZLIB_WBITS_MAX 12
#endif

static bool jl_inflate_raw_deflate(const uint8_t *src, size_t src_len, uint8_t wbits,
                                   uint8_t *dst, size_t dst_len) {
  uzlib_uncomp_t d;
  memset(&d, 0, sizeof(d));

  d.source = src;
  d.source_limit = src + src_len;
  d.dest_start = dst;
  d.dest = dst;
  d.dest_limit = dst + dst_len;
  d.checksum_type = UZLIB_CHKSUM_NONE;

  // Single small static dictionary window; clamp caller's wbits to this.
  static uint8_t window[1u << JL_STARTUP_UZLIB_WBITS_MAX];
  const uint8_t eff_wbits = (wbits > JL_STARTUP_UZLIB_WBITS_MAX) ? JL_STARTUP_UZLIB_WBITS_MAX : wbits;
  const size_t window_len = (size_t)1u << eff_wbits;
  uzlib_uncompress_init(&d, window, (unsigned int)window_len);

  int res;
  do {
    res = uzlib_uncompress(&d);
  } while (res == UZLIB_OK);

  if (res != UZLIB_DONE) {
    return false;
  }
  return (size_t)(d.dest - d.dest_start) == dst_len;
}
#endif

const uint32_t *jl_get_startup_frame(int imageIndex) {
  if (imageIndex < 0) {
    imageIndex = 0;
  } else if (imageIndex >= startupFrameLEN) {
    imageIndex = startupFrameLEN - 1;
  }

#if !JL_USE_COMPRESSED_STARTUP_FRAMES
  return startupFrameArray[imageIndex];
#else
  static_assert(sizeof(uint32_t) == 4, "startup frame expects 32-bit uint32_t");
  static uint32_t frame_buf[STARTUP_FRAME_U32_LEN];
  static int cached_index = -1;

  if (cached_index == imageIndex) {
    return frame_buf;
  }

  const DeflateBlob *blob = &startupFrameDeflateBlobs[imageIndex];
  const uint8_t *src = blob->data;
  const size_t src_len = blob->len;
  const uint8_t wbits = blob->wbits;

  const bool ok = jl_inflate_raw_deflate(src, src_len, wbits,
                                        (uint8_t *)frame_buf,
                                        (size_t)STARTUP_FRAME_U32_LEN * 4u);
  if (!ok) {
    memset(frame_buf, 0, sizeof(frame_buf));
  }

  cached_index = imageIndex;
  return frame_buf;
#endif
}


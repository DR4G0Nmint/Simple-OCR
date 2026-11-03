// JPEG compression artefacts modelled directly: 8x8 blocks are transformed with
// a forward DCT, quantized with a scaled standard luminance table, then
// inverse-transformed. This reproduces ringing and blocking from the codec
// itself rather than round-tripping through a JPEG encoder.
#pragma once

#include "common/image.h"

namespace ocr {

// `quality` follows the usual 1..100 scale (lower = stronger artefacts).
// Blocks are processed independently per channel; edge blocks are padded by
// replication.
void applyJpegArtifacts( Image& image, int quality );

} // namespace ocr

// SPDX-License-Identifier: GPL-3.0-or-later
// Internal encoder hooks for experiments (tools/quality); not part of the public API.
#pragma once

#include <mbevocoder/mbevocoder.h>

#include "ambe_quant.hpp"

namespace mbe {

// Analyse the next frame at the given fundamental period (samples, 21..122.5) instead of the
// tracker's own refined pitch; 0 leaves the tracker alone. One frame only.
void encoderForcePitch(mbe_encoder* e, float periodSamples);

// Replace parts of the next frame's quantiser input with reference values; null clears. One
// frame only. AMBE rates: the fields as documented in QuantizerOverride. IMBE (the default
// build's analysis path): b1 >= 0 takes the reference's per-harmonic voicing (voicedL, voiced,
// on the grid w0), haveGamma its mean log2Ml, L > 0 its shape (log2Ml, mean-removed), each
// resampled onto the frame's own harmonics; a forced pitch (encoderForcePitch) is kept.
void encoderForceModel(mbe_encoder* e, const QuantizerOverride* ov);

// The last frame as the analysis saw it after the front end (echo cancellation, noise
// suppression): for listening to and measuring the front end on its own.
void encoderFrontEndOutput(const mbe_encoder* e, short pcm[160]);

// Whether a layout carries the rate's frame (the D-STAR layout a 3600x2400 frame, DMR and
// P25 Phase 2 a 3600x2450 one, the 144-bit layouts an IMBE one).
bool layoutMatchesRate(mbe_rate r, mbe_layout layout);

} // namespace mbe

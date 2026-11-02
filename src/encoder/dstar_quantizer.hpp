// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace mbe {
struct AmbeRateTables;

// Refine b3..b8 against harmonic-domain, mean-free prediction residuals.
// residual and target contain L entries; b0..b2 are preserved. Internal to
// the experimental D-STAR encoder; callers supply a valid D-STAR model.
void refineDstarShape(const AmbeRateTables& tables, int L, const float* residual,
                      const float* target, int b[9]);
} // namespace mbe

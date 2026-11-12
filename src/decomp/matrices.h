// =============================================================================
// decomp/matrices.h -- Radical's matrix helpers + xnContext::UpdateTransforms
// as C++, bit-identical (decomp/matrices.cpp, findings/60)
// =============================================================================
#pragma once

namespace decomp_matrices {
// Installs the UpdateTransforms handler (the matrix helpers are plain
// overrides and need nothing). Call once at start, like the other PDDI handlers.
void Install();
}  // namespace decomp_matrices

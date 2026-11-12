// =============================================================================
// decomp/simple_setup.h -- xnSimpleShader::SetupDraw (sub_82441EA8) as C++
// (simple_setup.cpp has the whole story; docs/findings/60 s.10)
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace decomp_simple {

// In place of `original(ctx, base)` for sub_82441EA8: ours, the original
// (switch off / a frame being traced / an unknown virtual slot) or both
// compared (--debug_native_simple_check). Called from the one PDDI handler
// that function has (players/outfits.cpp HairSetupDraw: a function has ONE
// handler, and the hair / coin swaps must run first).
void SetupDraw(PPCContext& ctx, uint8_t* base, PPCFunc* original);

}  // namespace decomp_simple

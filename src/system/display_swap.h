// display_swap.h -- D3D's display gamma ramp, kept by the port (graphics driver step 3)
//
// See display_swap.cpp: D3D's ramp writer sub_82313960 is wrapped and its
// 256 entries copied, packed like the GPU's DC_LUT_30_COLOR register
// (red << 20 | green << 10 | blue, 10 bits each) = the layout the native
// renderer's present pass uses.
#pragma once

#include <array>
#include <cstdint>

namespace display_swap {

// D3D's current ramp. false = none seen yet, or --display_swap_native=false
// (then the caller falls back to the emulated GPU's copy). `version` (if
// given) changes each time D3D sends a new ramp.
bool GammaRamp(std::array<uint32_t, 256>& out, uint32_t* version = nullptr);

}  // namespace display_swap

#!/usr/bin/env python3
"""
gen_pddi_table.py -- list the renderer functions we intercept.

WHY
---
src/pddi/intercept.cpp wraps every call into Radical's renderer (PDDI) and
into Microsoft's D3D (docs/findings/08) by defining a strong `sub_XXXXXXXX`
that overrides the generated weak one. The frame tracer (src/pddi/trace.cpp)
and the native renderer (src/native/) both work through those wrappers.
This script decides WHICH functions, and writes them as an X-macro table:
src/pddi/functions.inc (addresses and short names only, no game code or
data).

WHAT GETS WRAPPED
-----------------
1. Every virtual method of the renderer classes (found through RTTI, see
   tools/rtti_vtables.py):
     pddi*          the engine's abstract renderer interface + generic parts
     xn*            its Xbox 360 backend (xnContext, xnTexture, xnShader...)
     FramebufferFxExt, RenderStrategyBink, PlayerBink
                    classes right on top of PDDI (framebuffer effects, the
                    Bink movie player), in the same code block
   Only methods inside the renderer code block 0x82420000-0x82460000 are
   kept: shared tiny stubs elsewhere (e.g. "return 0" used by hundreds of
   classes) would just be noise. _purecall is skipped.
2. Every function in Microsoft's D3D library called DIRECTLY (bl or tail
   call) from anywhere in the renderer block, including the non-virtual
   helpers there (the draw call, for one, sits in helper 0x824372E0); the
   trace's nesting still shows which method it ran under. D3D sits at
   0x82305000-0x82323000, with its draw functions in a separate block at
   0x82476000-0x82478000. The CRT/XAPI helpers just below 0x82305000
   (critical sections, SetLastError...) are left out. As found 2026-09-25,
   D3D has no callers outside PDDI.

NAMES
-----
Default name: "<class>::v<slot>" for the class that INTRODUCES the function
at that slot (its base class has something else there), e.g. xnContext::v4.
Methods inherited unchanged keep the base's name. D3D functions are
"d3d_XXXXXXXX". KNOWN_NAMES below overrides both, as we identify functions:
add a name there with a comment on how we know, then re-run this script.
A trailing '?' marks a name we believe but haven't verified.

USAGE
-----
    python3 tools/gen_pddi_table.py out/default_image.bin \\
        generated/default/crash_mom_init.cpp game/default.xex > src/pddi/functions.inc
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from callgraph import CallGraph  # noqa: E402
from rtti_vtables import Rtti  # noqa: E402

RENDERER_BLOCK = (0x82420000, 0x82460000)
D3D_BLOCKS = [(0x82305000, 0x82323000), (0x82476000, 0x82478000)]
PURECALL = 0x82459578
CLASS_PREFIXES = ("xn", "pddi", "FramebufferFxExt@", "RenderStrategyBink@", "PlayerBink@")

# Functions we already override elsewhere in src/ (a second strong definition
# would not link). frame_rate.cpp wraps the D3D flip-target function.
EXCLUDE = {0x82310728}

# Names we know. Keep the "how we know" comment next to each.
KNOWN_NAMES = {
    # findings/07: reads the vsync mode at this+1660, waits on the vblank
    # counter, then presents through D3D. Called once per frame.
    0x82433510: "xnDisplay::SwapBuffers",
    # findings/07: registers the vblank callback 0x82433380 through D3D.
    0x82433B00: "xnDisplay::InitDisplay",
    # findings/07: D3D's present, called by SwapBuffers with the frontbuffer.
    0x82310DA8: "D3D::Swap",
    # findings/07: SetVerticalBlankCallback (the only call from InitDisplay
    # that takes the callback's address).
    0x82311388: "D3D::SetVerticalBlankCallback",

    # --- From the first PDDI traces (findings/08, 2026-09-25) ---------------
    # First renderer call after every swap: sets the 1280x720 colour + depth
    # targets, then starts 3-strip tiling. Last call before the swap resolves
    # the strips and ends tiling.
    0x82431470: "xnContext::BeginFrame",
    0x82431F40: "xnContext::EndFrame",
    # Its only D3D call is D3D::Clear? below; the game calls it right after
    # setting up each render target.
    0x824306A0: "xnContext::Clear",
    # Immediate-mode geometry (HUD, menus, sprites): BeginPrims returns a
    # prim-stream object; the D3D call under it hands out a pointer into the
    # command buffer where vertices are written; EndPrims closes it.
    0x82430780: "xnContext::BeginPrims",
    0x82430870: "xnContext::EndPrims",
    # Called with 0x107 and returns the xnExtUnLitColourTint object.
    0x82430E60: "xnContext::GetExtension?",
    # Return 1280 / 720 in r3, 16/9 in f1 (the only float returned).
    0x82433EE8: "xnDisplay::GetWidth?",
    0x82433EE0: "xnDisplay::GetHeight?",
    0x82433970: "xnDisplay::GetAspect?",
    # Draws a static mesh: SetIndices?, SetVertexDeclaration?,
    # SetStreamSource?, then DrawIndexedVertices? (289x in a hub frame).
    0x82443BD0: "xnPrimBuffer::Draw?",
    # Material parameters are four-letter IDs: r4 = 0x00584554 ("TEX") with a
    # texture object, r4 = 0x0054494C ("LIT") with an integer.
    0x8243BAC8: "pddiBaseShader::SetTexture?",
    0x8243BB80: "pddiBaseShader::SetInt?",

    # D3D (Microsoft's library), named from arguments and call order.
    0x8230DD40: "D3D::SetRenderTarget?",       # (index 0/1, surface, ...)
    0x8230DA68: "D3D::SetDepthStencilSurface?",
    0x823193D0: "D3D::BeginTiling?",           # count 3 + an array of strip rects
    0x82319860: "D3D::EndTiling?",             # resolves each strip itself
    0x82319260: "D3D::SetPredication?",        # masks 2/8/0x20 = strip 0/1/2
    0x8231EE00: "D3D::Resolve?",               # EDRAM -> texture copy
    0x82322808: "D3D::Clear?",                 # flags 0x3F = all targets + depth + stencil
    0x82476DA0: "D3D::BeginVertices?",         # (prim type, count, stride) -> write pointer
    0x82477238: "D3D::EndVertices?",
    0x82477B58: "D3D::DrawIndexedVertices?",   # (prim type, base, start, index count)
    0x8230CF78: "D3D::SetIndices?",
    0x8230CE58: "D3D::SetStreamSource?",       # (stream 0, buffer, offset, stride 24)
    0x82313040: "D3D::SetVertexDeclaration?",
    0x82312E70: "D3D::SetVertexShader?",
    0x82312BB8: "D3D::SetPixelShader?",
}


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    g = CallGraph(*sys.argv[1:4])
    rtti = Rtti(g.img, g.code_ranges)
    named = rtti.by_name()

    def in_block(a, blocks):
        return any(lo <= a < hi for lo, hi in blocks)

    # --- 1. renderer methods, named after the class that introduces them ---
    methods = {}  # address -> name
    for name, cls in sorted(named.items()):
        if not name.startswith(CLASS_PREFIXES):
            continue
        short = name.split("@")[0]
        parent = named.get(cls["bases"][1][0]) if len(cls["bases"]) > 1 else None
        pslots = next((c["slots"] for c in parent["cols"] if c["offset"] == 0), []) if parent else []
        for col in cls["cols"]:
            for i, fn in enumerate(col["slots"]):
                if fn == PURECALL or fn in EXCLUDE or not in_block(fn, [RENDERER_BLOCK]):
                    continue
                if fn not in g.start_set:
                    # Not a function the recompiler registered: no
                    # __imp__sub_ to wrap (would not link). Report it.
                    print(f"// warning: {short}::v{i} = 0x{fn:08X} is not a "
                          "registered function", file=sys.stderr)
                    continue
                introduced = col["offset"] != 0 or i >= len(pslots) or pslots[i] != fn
                if introduced and fn not in methods:
                    sub = f"+{col['offset']}" if col["offset"] else ""
                    methods[fn] = f"{short}{sub}::v{i}"
    # Inherited-only entries (no class introduced them in our set): name by
    # the first class that has them.
    for name, cls in sorted(named.items()):
        if not name.startswith(CLASS_PREFIXES):
            continue
        for col in cls["cols"]:
            for i, fn in enumerate(col["slots"]):
                if (fn not in methods and fn != PURECALL and fn not in EXCLUDE
                        and fn in g.start_set and in_block(fn, [RENDERER_BLOCK])):
                    methods[fn] = f"{name.split('@')[0]}::v{i}"

    # --- 2. D3D functions called directly from the renderer block ---
    d3d = set()
    for fn in g.starts:
        if not in_block(fn, [RENDERER_BLOCK]):
            continue
        for callee in g.callees.get(fn, ()):
            if in_block(callee, D3D_BLOCKS) and callee not in EXCLUDE:
                d3d.add(callee)

    print("// =============================================================================")
    print("// src/pddi/functions.inc -- GENERATED by tools/gen_pddi_table.py")
    print("// =============================================================================")
    print("// Do not edit by hand: add names to KNOWN_NAMES in the script and re-run it.")
    print("// One line per wrapped function: X(hex address, \"name\").")
    print("//   PDDI_METHOD  a virtual method of a renderer class (pddi*, xn*, ...)")
    print("//   PDDI_D3D     a D3D library function called directly from the renderer block")
    print("//   PDDI_VTABLE  a renderer class's main vtable (names `this` in traces)")
    print(f"// {len(methods)} methods, {len(d3d)} D3D functions.")
    print("// =============================================================================")
    print()
    for fn in sorted(methods):
        print(f'PDDI_METHOD({fn:08X}, "{KNOWN_NAMES.get(fn, methods[fn])}")')
    print()
    for fn in sorted(d3d):
        print(f'PDDI_D3D({fn:08X}, "{KNOWN_NAMES.get(fn, f"d3d_{fn:08X}")}")')
    print()
    for name, cls in sorted(named.items()):
        if name.startswith(CLASS_PREFIXES):
            for col in cls["cols"]:
                if col["offset"] == 0:
                    print(f'PDDI_VTABLE({col["vtable"]:08X}, "{name.split("@")[0]}")')


if __name__ == "__main__":
    main()

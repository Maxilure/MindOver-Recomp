#!/usr/bin/env python3
# =============================================================================
# tools/make_player_pictures.py -- the Players screen's pictures, baked once
# =============================================================================
#
# WHAT: the Players screen (src/players/players_setup.h) shows each player's
# controller FILLED in that player's colour with a navy edge, a faint
# controller on an empty card, a selection ring round a card and an opaque
# teal backing under each card's glass (the screen's design, 2026-10-29).
# This script makes those PNGs from Xelu's controller outlines (CC0,
# assets/prompts/controllers/*.png) into assets/prompts/controllers/players/.
# The game only loads them (players_setup.cpp MenuSprites).
#
# HOW the outlines are filled (a first version filled the 128-pixel outlines
# directly: hard, uneven edges): at 4x the size, the outline is thickened a
# touch (even line weight), everything not reached by a flood fill from the
# picture's edge is the BODY (filled in the colour), the lines stay navy with
# their soft edges kept, then the result is scaled down by averaging (smooth
# edges). Parts drawn as SOLID white shapes instead of outlines (the Switch's
# right Joy-Con) are found by erosion (thicker than any line) and filled
# too, with a navy rim. The Switch uses the pack's Joy-Cons drawn apart, with
# light inner lines (the look picked from four, 2026-10-29).
#
# The rings, the card backing and the corner glows (aura_p<N>: each player's
# colour from their outer corner) are the port's own simple drawings, not
# from any pack.
#
# Run: python3 tools/make_player_pictures.py   (needs Pillow + numpy)
# =============================================================================
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "prompts" / "controllers"
OUT = SRC / "players"

NAVY = np.array([0x14, 0x2F, 0x5E], float)
# Players 1-4 (src/players/players_setup.h kPlayerColour).
COLOURS = [(0xFF, 0x5A, 0x3C), (0x3B, 0xA7, 0xFF), (0x3C, 0xD2, 0x3C), (0xB4, 0x5A, 0xF0)]
PADS = ["xbox", "ps5", "ps4", "switch"]
# The source picture of each (Switch: the pack's two Joy-Cons drawn apart,
# the look picked for the cards; 100 px in the pack, scaled to 128).
SOURCES = {"switch": "switch-apart"}
# Pads whose INNER lines (buttons, sticks, the Joy-Con rails) are drawn in a
# light shade of the player's colour instead of navy, only the outer edge
# navy: on the Joy-Cons a navy line round every button looked busy.
LIGHT_INSIDE = {"switch"}
SCALE = 4  # work at 4x, then average down


def filled(name, colour, opacity=1.0, light_inside=False):
    """Xelu outline `name` -> body in `colour`, navy lines, smooth edges
    (`light_inside`: only the outer edge navy, the inner lines a light shade)."""
    im = Image.open(SRC / f"{name}.png").convert("RGBA")
    if im.size != (128, 128):
        im = im.resize((128, 128), Image.LANCZOS)
    w, h = im.size
    big = im.resize((w * SCALE, h * SCALE), Image.LANCZOS)
    ink = big.getchannel("A")
    line_mask = ink.point(lambda v: 255 if v > 128 else 0)
    # SOLID shapes: what survives an erosion wider than a line.
    solid = line_mask.filter(ImageFilter.MinFilter(17)).filter(ImageFilter.MaxFilter(17))
    rim_kept = np.asarray(solid.filter(ImageFilter.MinFilter(13))) > 128  # inside, minus a navy rim
    # Line ink, a touch thicker (even weight), none inside solid shapes.
    a = np.asarray(ink.filter(ImageFilter.MaxFilter(3))).astype(float) / 255
    a = np.where(rim_kept, 0.0, a)
    # The outside: flood fill from the edge (one pixel of room all round).
    barrier = Image.fromarray((((a > 0.5) | rim_kept) * 255).astype(np.uint8))
    pad = Image.new("L", (w * SCALE + 2, h * SCALE + 2), 0)
    pad.paste(barrier, (1, 1))
    ImageDraw.floodfill(pad, (0, 0), 128)
    outside = np.asarray(pad)[1:-1, 1:-1] == 128
    body = np.asarray(Image.fromarray(((~outside) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2)))
    body = body.astype(float) / 255
    col = np.array(colour, float)
    if light_inside:
        # The outer edge = line ink within about a line's width of the outside.
        near_out = np.asarray(Image.fromarray((outside * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(25))) > 128
        outer = np.where(near_out, a, 0.0)
        inner = a - outer
        light = col + (255 - col) * 0.6
        rgb = col * (1 - a[..., None]) + light * inner[..., None] + NAVY * outer[..., None]
    else:
        rgb = col * (1 - a[..., None]) + NAVY * a[..., None]
    alpha = np.maximum(a, body) * opacity
    out = Image.fromarray(np.dstack([rgb, alpha * 255]).round().astype(np.uint8), "RGBA")
    return out.resize((w, h), Image.BOX)


def rounded(w, h, radius, fill=None, bands=()):
    """A rounded rectangle at 4x, averaged down: `fill` = a function of the
    row (0..1) giving RGBA, or `bands` = (inset, thickness, RGBA) outside-in."""
    W, H, R = w * SCALE, h * SCALE, radius * SCALE
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    if fill:
        grad = Image.new("RGBA", (W, H))
        for y in range(H):
            grad.paste(fill(y / (H - 1)), (0, y, W, y + 1))
        mask = Image.new("L", (W, H), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1), R, fill=255)
        img.paste(grad, (0, 0), mask)
    for inset, thickness, rgba in bands:
        i, t = inset * SCALE, thickness * SCALE
        mask = Image.new("L", (W, H), 0)
        d = ImageDraw.Draw(mask)
        d.rounded_rectangle((i, i, W - 1 - i, H - 1 - i), max(1, R - i), fill=255)
        d.rounded_rectangle((i + t, i + t, W - 1 - i - t, H - 1 - i - t), max(1, R - i - t), fill=0)
        img.paste(Image.new("RGBA", (W, H), rgba), (0, 0), mask)
    return img.resize((w, h), Image.BOX)


def aura(colour, corner):
    """A soft glow of `colour` from one corner of a card (0 top left, 1 top
    right, 2 bottom left, 3 bottom right = players 1-4: each player's outer
    corner of the screen), fading out a little past a third of the card and
    kept inside its rounded shape. Drawn over the card's glass."""
    w, h, radius = 240, 160, 12
    W, H = w * SCALE, h * SCALE
    cx = 0 if corner in (0, 2) else W - 1
    cy = 0 if corner in (0, 1) else H - 1
    y, x = np.mgrid[0:H, 0:W].astype(float)
    # Distance from the corner, the card's shape squashed to a circle-ish
    # spread (wider than tall: the card is 3:2).
    d = np.hypot((x - cx) / (W * 0.62), (y - cy) / (H * 0.78))
    a = np.clip(1 - d, 0, 1) ** 1.8 * 0.80
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1), radius * SCALE, fill=255)
    a = a * (np.asarray(mask).astype(float) / 255)
    rgb = np.broadcast_to(np.array(colour, float), (H, W, 3))
    img = Image.fromarray(np.dstack([rgb, a * 255]).round().astype(np.uint8), "RGBA")
    return img.resize((w, h), Image.BOX)


def teal(t):
    """The cards' backing: #EAFCFB -> #B5EBEA (55 %) -> #93DCDD, top to bottom."""
    a, m, b = (0xEA, 0xFC, 0xFB), (0xB5, 0xEB, 0xEA), (0x93, 0xDC, 0xDD)
    if t < 0.55:
        c = [round(a[k] + (m[k] - a[k]) * t / 0.55) for k in range(3)]
    else:
        c = [round(m[k] + (b[k] - m[k]) * (t - 0.55) / 0.45) for k in range(3)]
    return tuple(c) + (255,)


def main():
    OUT.mkdir(exist_ok=True)
    for p, colour in enumerate(COLOURS):
        for pad in PADS:
            filled(SOURCES.get(pad, pad), colour, light_inside=pad in LIGHT_INSIDE).save(
                OUT / f"{pad}_p{p + 1}.png", optimize=True)
    filled("xbox", (255, 255, 255), 0.5).save(OUT / "empty.png", optimize=True)
    navy = (0x14, 0x2F, 0x5E, 255)
    ring_colours = [(0xFD, 0xED, 0x06, 255)] + [c + (255,) for c in COLOURS]
    for i, c in enumerate(ring_colours):
        name = "ring_selected.png" if i == 0 else f"ring_p{i}.png"
        rounded(240, 160, 16, bands=((0, 2.5, navy), (2.5, 3, c))).save(OUT / name, optimize=True)
    rounded(240, 160, 12, fill=teal).save(OUT / "card_back.png", optimize=True)
    for p, colour in enumerate(COLOURS):
        aura(colour, p).save(OUT / f"aura_p{p + 1}.png", optimize=True)
    print(f"wrote {len(list(OUT.glob('*.png')))} pictures to {OUT}")


if __name__ == "__main__":
    main()

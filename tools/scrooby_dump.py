#!/usr/bin/env python3
"""
scrooby_dump.py -- print the menu / HUD layouts ("Scrooby" projects) of a
Pure3D package as a tree or as JSON (docs/findings/30).

WHAT IS SCROOBY?
----------------
Radical's 2D front-end library (namespace pure3d::frontend in the game's RTTI:
Project, Screen, Page, Layer, Group, Menu, MenuItem, Text, Sprite, Polygon).
Every menu, message box and most HUD texts are DATA: a "project" chunk inside
a Pure3D package (.p3d in default.rcf; tools/... or notes' rcf_extract pulls
them out). The game's code only finds elements BY NAME and changes them
(text, visibility, position, menu selection).

FORMAT (Pure3D chunks, LITTLE-endian inside packages)
-----------------------------------------------------
Every chunk = u32 id, u32 data size (header + own fields), u32 total size
(+ children), then its fields, then child chunks. Names are a length byte +
that many bytes (NUL padded). The ones that matter here:

  0x18000 project  "InGame.prj"  : u32 version, u32 width 640, u32 height 480,
                                   platform + folder strings
  0x18001 screen   "InGame.scr"  : u32 version, u32 count, page names
  0x18002 page     "...pag"      : u32 version, u32 640, u32 480
  0x18020 layer                  : u32 version, u32 visible, ...
  0x18010 menu                   : a vertical list of items (selected colour,
                                   wrap flag); children = items
  0x18011 menu item              : child 0x18013 = what is drawn, optional
                                   0x18014 = the item's VALUE (a text whose
                                   strings are the choices: Off / On ...)
  0x18023 text     : u32 version 18, i32 x, i32 y, i32 w, i32 h, u8 ?,
                     u32 horizontal justify (0 left, 1 right, 4 centre),
                     u32 vertical justify, u32 colour (bytes B G R A), u32,
                     f32 rotation, font name (Titans_Small / Titans_Large),
                     ...; children 0x1800B = (text bank, string id) per choice
  0x18022 picture  : u32 version 2, x, y, w, h, u32 frames, u8 ?, u32
                     justify, u32 colour (B G R A: alpha = translucency),
                     u32, f32, u32 count, picture names (one per FRAME, e.g.
                     a star's open / closed pictures, a save slot's 9 levels)
  0x18009 polygon  : u32 version, u32, u32 n, n x (f32 x, y, z), n colours
                     (flat quads, colour per corner)
  0x1800D font / text bank (glyphs + strings per language; not decoded here)
  0x19005 picture data (a PNG inside; skipped)

COORDINATES: every page is a 640 x 480 canvas, origin BOTTOM-LEFT, y UP, and
(x, y) is an element's bottom-left corner. The game draws that 4:3 canvas
centred on the 16:9 screen: at 1280 x 720, screen x = 160 + 1.5 x, screen
y = 720 - 1.5 y. Widescreen decorations sit at x < 0 or x > 640.

Usage:
  python3 tools/scrooby_dump.py <package.p3d>            tree of every page
  python3 tools/scrooby_dump.py <package.p3d> InGame_Pause.pag ...   some pages
  python3 tools/scrooby_dump.py --json <package.p3d>      everything as JSON
"""
import json
import struct
import sys

KIND = {0x18001: 'screen', 0x18002: 'page', 0x18020: 'layer', 0x18010: 'menu', 0x18011: 'item',
        0x18013: 'item-look', 0x18014: 'item-value', 0x18022: 'picture', 0x18023: 'text',
        0x18009: 'polygon', 0x1800B: 'string'}
SKIP = (0x19005, 0x1800D)  # picture data and fonts: big, not layout


def chunks(d, b, e):
    """Yield (id, start, data size, total size) of the chunks between b and e."""
    p = b
    while p + 12 <= e:
        cid, ds, ts = struct.unpack('<3I', d[p:p + 12])
        if ts < 12 or ds < 12 or ds > ts or p + ts > e:
            break
        yield cid, p, ds, ts
        p += ts


def lstr(d, o):
    """A length-prefixed string at o -> (text, offset after it)."""
    n = d[o]
    return d[o + 1:o + 1 + n].split(b'\0')[0].decode('latin1'), o + 1 + n


def u32(d, o): return struct.unpack('<I', d[o:o + 4])[0]
def i32(d, o): return struct.unpack('<i', d[o:o + 4])[0]
def colour(d, o): return '#%02x%02x%02x%02x' % (d[o + 2], d[o + 1], d[o], d[o + 3])  # stored B G R A -> #rrggbbaa


def element(d, cid, p, ds, ts):
    nm, o = lstr(d, p + 12)
    e = {'type': KIND.get(cid, '%X' % cid), 'name': nm}
    if cid in (0x18023, 0x18022):
        e['x'], e['y'], e['w'], e['h'] = (i32(d, o + 4 + 4 * k) for k in range(4))
        o += 20
        if cid == 0x18023:
            o += 1
            e['justify'] = [u32(d, o), u32(d, o + 4)]
            e['colour'] = colour(d, o + 8)
            e['font'], o = lstr(d, o + 20)
        else:
            e['frames'] = u32(d, o)
            e['colour'] = colour(d, o + 9)
            o += 21
            e['pictures'] = []
            for _ in range(u32(d, o)):
                s, o = lstr(d, o + 4 if not e['pictures'] else o)
                e['pictures'].append(s)
    elif cid == 0x18009:
        n = u32(d, o + 8)
        o += 12
        e['corners'] = [[round(v) for v in struct.unpack('<3f', d[o + 12 * k:o + 12 * k + 12])[:2]] for k in range(n)]
        e['colours'] = [colour(d, o + 12 * n + 4 * k) for k in range(n)]
    elif cid == 0x1800B:
        e['bank'] = nm
        e['id'], _ = lstr(d, o)
    elif cid == 0x18001:
        e['pages'] = []
        o += 8
        for _ in range(u32(d, o - 4)):
            s, o = lstr(d, o)
            e['pages'].append(s)
    kids = [element(d, c, q, cds, cts) for c, q, cds, cts in chunks(d, p + ds, p + ts) if c not in SKIP]
    if kids:
        e['children'] = kids
    return e


def projects(path):
    d = open(path, 'rb').read()
    out = []
    for cid, p, ds, ts in chunks(d, 12, len(d)):
        if cid == 0x18000:
            out.append({'project': lstr(d, p + 12)[0],
                        'children': [element(d, c, q, cds, cts) for c, q, cds, cts in chunks(d, p + ds, p + ts)]})
    return out


def show(e, depth=0):
    s = '  ' * depth + e['type'] + ' ' + e.get('name', '')
    if 'x' in e:
        s += ' @(%d,%d) %dx%d' % (e['x'], e['y'], e['w'], e['h'])
    for k in ('colour', 'font'):
        if k in e:
            s += ' ' + e[k]
    if 'justify' in e:
        s += ' justify %s' % e['justify']
    if 'pictures' in e:
        s += ' ' + ','.join(e['pictures'])
    if 'id' in e:
        s += ' "%s"' % e['id']
    if 'pages' in e:
        s += ' ' + ' + '.join(e['pages'])
    if 'corners' in e:
        s += ' %s %s' % (e['corners'], e['colours'][0])
    print(s)
    for c in e.get('children', []):
        show(c, depth + 1)


if __name__ == '__main__':
    args = sys.argv[1:]
    as_json = '--json' in args
    args = [a for a in args if a != '--json']
    if not args:
        sys.exit(__doc__)
    found = projects(args[0])
    if as_json:
        print(json.dumps(found, indent=1))
    for pr in ([] if as_json else found):
        print('project', pr['project'])
        for c in pr['children']:
            if len(args) == 1 or c['name'] in args[1:]:
                show(c, 1)

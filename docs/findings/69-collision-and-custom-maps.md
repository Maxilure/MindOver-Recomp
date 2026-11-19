# 69. Collision meshes on disc: the road to a test course and custom maps

2026-11-19. Research start. **Why:** checking frame-rate fixes on real game
spots is slow: the bugs depend on a level's own triangles, spots have to be
found from traces, and climbs and ledges are hard to reach by script. A
**test course** (slopes of known angles, steps of known heights, ledges,
later climbables) would let the probe kit (findings/22 s.10) sweep each
quirk at 30 / 60 / 120 / 180 fps in a controlled way. It is also the first
step towards custom maps (roadmap phase 5, mods). The format is decoded and a
tree builder exists (scratch tools); nothing is in the game yet.

## 1. What a level is made of

A level folder in `default.rcf` (`levels/L0` = Crash's house, `L1`...`L9`)
holds only small files: scripts (`objectives`, `props_*`, `spawn_*`,
`sound_*`, `streamgraph`, `NonSpawnableTemplateActor`, `cameraoverrides`;
Lua 5.1 bytecode, findings/19 s.1), `cameradata.p3d`, `Skybox.p3d` and
`LevelPackages.p3d`. The last one lists the level's packages by name, e.g.
for L0 `L0/aec5f73c` ("joined_assets") and `L0/4463ada` ("room", L0_R1);
the packages themselves are `package/<level>/<name>.p3d`.
`levels/GlobalPackagesNoHash.p3d` keeps readable names for the global list.

Pure3D packages are chunk trees, **little-endian** after the `P3D\xff` file
header: `u32 id, u32 own size (12-byte header + own data), u32 total size
(+ children)`. The house room package holds textures (`0x19000`), materials
(`0x11000`), meshes (`0x10000`, e.g. `roomShape`, `tableShape`), effects,
a scenery root (`0x121000`) and **one collision object**, `room_collision`.

## 2. The collision object

| Chunk | Contents (house room: 286 triangles) | Loaded into (mesh object) |
|---|---|---|
| `07010025` | collision object: name, then children | `sub_8244DCF0` / `sub_82450DE0` |
| `07010001` | 12 bytes (zero in the house), holds the mesh | |
| `0701000A` | mesh header: u32 5, 2 u32, box min + max (6 floats) | mesh `+32` min, `+44` max; `+56/+60/+64` = 1/size per axis (computed) |
| `07000003` | u32 count, corners (3 floats each); 145 | `+8` corners, `+24` u16 count |
| `07000006` | u32 count, triangles: u16 corners x3 + u16 flags; 286 | `+16`, 8 bytes each (16 with `+28` set: then 3 more u16 from `07000005`), `+26` u16 count; flags rotated by 4 bits on load |
| `07000008` | u32 count, u16 words; 1,754 | `+12`: the **search tree** over the triangles (section 3) |
| `07000748` | u32 count, 36-byte entries, one per triangle | `+20`, 12 bytes each: 3 u16 = the triangles across each edge (the neighbours), `+6` = 0xFFFF, `+11` = a byte scaled from the 7th word; 3 floats (1.9 in the house) skipped |
| `07016000` | surface materials (`07016012` "SurfaceMaterialData": per-triangle numbers) | through a registered callback (`[0x82597390]` area) |

The mesh loader is `sub_82450140` (chunk loop: `sub_82355848` = next chunk
id, `sub_82354FA0` = read). The in-memory layout matches what the ground
fix reads in a running game (findings/22 s.11: `+0x08` corners, `+0x10`
triangles, `+0x18`/`+0x1A` counts, `+0x1C` the 16-byte flag). The `07000748`
neighbour list is the game's own triangle adjacency, which the ground fix
currently rebuilds by sharing corners.

## 3. The search tree, decoded

Static searches for the walker found nothing (the mesh pointer is handed
along first), so it was caught in the act: a hardware read watchpoint
(gdb `rwatch`) on two words of Crash's ground mesh tree on Wumpa Island,
armed from a breakpoint on the reach check `sub_8217D668`. First reader:
`sub_8244E760`, called by `sub_8244E5F0` <- `sub_8244D838` <- `sub_8224ECF8`.
(A SIGINT to the game doesn't reach gdb: the game blocks it.)

**The grid.** `sub_8244BA60` turns a point into three u16:
`trunc((p - box min) x 65535 / (box max - box min))`, clamped to 0..65535 per
axis (the scales are the loader's `+56/+60/+64`). The query
(`sub_8244E5F0`) makes the grid box of what it looks for, widens it by 2
steps on each side, then walks the tree.

**A node** is a run of u16 words starting with a head word `w`:

| Bits of `w` | Meaning |
|---|---|
| 14-15 | split axis: 0 x, 1 y, 2 z; **3 = leaf** |
| 12-13 | type: bit 0 = the LEFT side is an inline triangle list, bit 1 = the RIGHT side is one |
| rest, type 0 | `here` = w & 0xFFF |
| rest, type 1 | left count = (w >> 6) & 63, `here` = w & 63 |
| rest, type 2 | right count = (w >> 6) & 63, `here` = w & 63 |
| rest, type 3 | left = (w >> 8) & 15, right = (w >> 4) & 15, `here` = w & 15 |

After the head: `here` triangle numbers (always reported), then for a
non-leaf the **split** (a grid coordinate), then the left side (an inline
list of triangle numbers, or ONE word: the child's offset in words from
this node's head), then the right side the same way. The walk visits the
left side when the query's min on the axis is <= split, the right side when
its max is >= split (the right child is a loop, not a call). A triangle
reaching both sides of a split is listed on both.

**Checked** with `notes/scratch-tools/maps/coltree.py`: on the house and on
all 43 collision objects of Wumpa Island (30,369 triangles) the walk reads
every tree word, lists every triangle, and each triangle's own box finds
it. On the island some words are visited twice: identical subtrees are
shared, which the walker allows.

**Building one:** `coltree_build.py` makes a tree from any triangle list
(middle-of-box splits, type-0 nodes and leaves only, crossing triangles on
both sides, bigger leaves when the tree would pass 65,536 words: offsets are
u16). 3,000 random-box queries per mesh against brute force find every
overlapping triangle. Candidates per query: house 5.9 (disc tree 6.2), the
island's largest meshes 10-23 (disc 4.5-7.4); the disc's trees are tighter
because they also pack short triangle lists into the nodes (types 1-3).
Fine for a test course of a few hundred triangles; worth matching later
for whole maps.

## 4. The test course

`--test_course=true` (src/world/test_course.cpp) adds an invisible course to
Wumpa Island's piece `L1/9db06066` (a "room" package: LevelPackages type 14,
path category 15). The log lists each lane's start spot ("Test course: lane
ramp30 starts at 315.00 27.43 -398.00"), reached with `goto 1 <spot>`.

**What it is:** 10 lanes side by side (8 wide, 9 apart), each running along
+x: a 6-unit flat start pad, the feature, a 6-unit end pad at y 20, all on a
1-unit triangle grid. Ramps of 10, 20, 30, 40, 50 degrees (12 long), stairs
of 10 steps of 0.10, 0.25, 0.50, 1.00 units (1-unit treads, vertical risers),
a 15-degree slope with +-0.12 bumps per grid corner. A flat catch floor at
y 12 under everything. 4,848 triangles; the piece goes from 3,987 to 8,835,
its tree from 29,141 to 43,084 words. Placed at x 312-344, z -420..-331, in
the piece's box and clear of every Wumpa Island piece's collision (a first
place at x 300 stood half inside another piece, `L1/185bac9c`, which fills
x 290-310 at z -390..-340: Crash stopped dead at an invisible wall).

**What the patch writes**, in the game's own formats: corners (shared ones
merged, stored as 32-bit floats), triangles with the edge flags (bit k =
edge k open or a ridge: checked on the island's 3,987 triangles, where open
and ridge edges carry it and flat joins and valleys don't), the per-triangle
entries (neighbours across each edge, 1.9 x3, the normal), the surface
material 19 (the island's usual ground) in "SurfaceMaterialData", and a new
tree for the whole mesh. The tint / saturation streams list only some
triangles and are left alone. Nothing is drawn.

**Checked:** Crash stands on the pads at their exact heights (22.12 on the
10-degree ramp's pad, 30.00 on the top of the 1.0 stairs), with a ground
contact. The Python prototype (notes/scratch-tools/maps/course.py +
p3d_patch.py) and the C++ patch make the same bytes
(`--debug_test_course_dump`).

**Driving it:** out there no camera volume holds the camera, so the stick's
world direction differs per lane after a teleport. `maps/course.sh` runs every
lane in one session (exact replays, fixed step); `maps/aim.py` measures each
lane's heading from a 30 fps walk and turns that lane's stick to go straight
down it.

## 5. First results: slopes fixed, real steps not

Run (3 s, full stick) and walk (4 s, 0.73 stick) down every lane, ground
grace off, exact replays. "Short" = a ground loss under 150 ms dropping less
than 0.6 units (what the move logic hears as a split-second fall); falls off
the end pads are real and not counted.

| Short losses | 30 fps (original) | 60 fps | 180 fps | 60, no handover | 180, no handover |
|---|---|---|---|---|---|
| ramps 10-50° + bumpy, 12 runs | 2 | **0** | **0** | 9 | 17 |
| stairs, run | 12 (33-100 ms) | 19 (17-50 ms) | 20 (11-56 ms) | same | same |
| stairs, walk | 3 | 8 (17-83 ms) | 36 (mostly 6 ms) | same | same |

* **Slopes:** with the triangle handover (findings/22 s.11) the move logic
  hears fewer split-second losses at 60 and 180 fps than the original does
  at 30. Without it, the 20-degree ramp alone gives 9 at 180 fps.
* **Real steps:** every step down is a real short drop, at 30 fps too (the
  original loses the ground for one 33 ms frame at each 0.25 step when
  running). The handover doesn't touch them (same numbers without it). At
  high frame rates the losses are longer in time when running (50 ms against
  33) and, walking, come as several one-frame losses per step (0.25 stairs
  walked at 180 fps: 21 losses of 6 ms, at 30 fps: none). With the ground
  grace off, each is heard by the move logic. So the ground grace still has
  work on real stairs; the next fix belongs at the source, the step-down.

## 6. Next

1. ~~A copy of a real spot~~ (section 7).
2. A step-down fix at the source (when the reach check drops the contact
   and ground lies within a small step below, as at 30 fps where one frame
   covers the whole drop), checked on the stair lanes against 30 fps.
3. More lanes: ledges, a moving platform, steep "slide" slopes, an edge
   for titans; drawing the course (a plain mesh, or lines over the picture).
4. Climbables and other special surfaces (objects with their own
   behaviours, placed by level data), then a level of its own (packages,
   scripts, a spawn point, a `LevelPackages` entry).

## 7. A copy of a real spot: the Ratcicle Kingdom courtyard's descent

**Why:** the lanes are not like real ground. Walked in a straight line
(`maps/ground_compare.py`, a sample every 5 cm), the courtyard descent of
findings/22 and the lanes give:

| | courtyard descent | 20° / 30° ramp lanes | bumpy lane |
|---|---|---|---|
| a new triangle every | 0.44-0.51 units | 1.0 | 1.0 |
| steepness on the way | 0-54°, changing all the time | one angle | 0-24° |
| joins that bend (2° or more) | 16-20 of 20-30 | 1 (the top) | 16 of 16 |
| largest bend | 27-54° | the ramp's angle, once | 16° |

The ground losses of findings/22 s.10-11 happened at those bends. So the
course also holds a **copy of the descent**: every triangle of Ice Kingdom's
piece `L2/8514dde0` whose middle is in x 252..284, z -148..-104, read from
the player's disc at run time, with its own edge flags and surface
materials, moved by (+48, 0, -330) into free air south of the lanes. The
Wumpa piece's box grows to hold it (z down to -480, y -16..60, so the walls
around the descent, y -14 to 56, come along whole; no other Wumpa Island
piece reaches there). Edges cut at the border are open edges; a catch floor
at y -15.5. 574 triangles from 2,441; the slope probe's spot
(262.329, 31.5, -126.734) is (310.329, 31.5, -456.734).

**The copy matches:** along the three walks above, the same triangles,
slopes and bends (one walk meets one triangle more near the border); Crash
stands at the probe spot at the original's height (30.905).

**How it was compared:** the 16 walks of findings/22 s.11 (run and walk, 8
directions; there started right after leaving a titan) were walked on the
copy in the same WORLD directions: the stick angle measured once per spot
(`maps/headings.py`), then each walk tuned by its own heading error, to
within 1° for the walks. 13 of 16 end within about 1 unit of where the
original ends. Earlier tries showed what matters: a copy cut tight around
the path gave extra losses at its border (2 units from the start), and
without the walls (first cut at the Wumpa box's y 47) the runs left the
copy where the original stops at a wall.

Grace off, short losses (under 150 ms, under 0.6 units) / real falls, 16 walks:

| | 30 fps | 60 fps | 180 fps | 180 fps, no handover |
|---|---|---|---|---|
| the copy | 1 / 2 | **0** / 3 | **0** / 2 | 7 / 4 |
| the original (Ice Kingdom) | | 0 / 4 | 3 / 3 | 14 / 3 |

Without the triangle handover the copy trips on the same walks as the
original (the downhill runs). With it, both are clean at 60 fps; at 180 the
copy is clean and the original's 3 all come from one walk (w5) whose path
parts from the copy's early (the start differs: after a titan in the
original, a teleport here). Differences left: Crash's facing at the start,
and anything of the Ice Kingdom that isn't in that one piece (props,
other pieces' collision).

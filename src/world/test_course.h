// =============================================================================
// world/test_course.h -- an invisible TEST COURSE for frame-rate testing (dev setting, off by default)
// =============================================================================
//
// WHY (2026-11-19, docs/findings/69): frame-rate fixes (the ground contact
// above 30 fps, findings/22) were checked on the game's own spots, which are
// slow to find from traces and hard to reach by script. The course gives the
// probes (notes/scratch-tools/frame-probes, maps/course.sh) controlled ground:
// lanes of ramps at 10-50 degrees, stairs with 0.1-1.0 unit steps and a bumpy
// slope, all on a 1-unit triangle grid like real ground, over a catch floor.
//
// HOW: a data patch (data/data_patcher.h) of Wumpa Island's room package
// L1/9db06066 adds the course's triangles to its collision object
// (room_collision): corners, triangles with the game's edge flags, the
// neighbour/normal entries, surface materials, and a NEW search tree for the
// whole mesh (the format, findings/69 s.3). Built from the player's own disc
// file at run time. Placed in empty air inside that piece's box, clear of
// every Wumpa Island piece's collision; only collision: nothing is drawn.
// Plus a COPY of a real spot (the Ratcicle Kingdom courtyard's descent, from
// Ice Kingdom's piece L2/8514dde0), south of the lanes in the grown box.
//
// USE: --test_course=true, load a Wumpa Island save, then `goto 1 <spot>`
// with a spot from the log lines "Test course: lane <name> starts at ...".
// notes/scratch-tools/maps/course.py + p3d_patch.py are the Python prototype;
// they made the same bytes before the copy was added (checked with
// --debug_test_course_dump); the copy exists in C++ only.
// =============================================================================
#pragma once

namespace test_course {

// Registers the data patch when --test_course is on (before data_patcher::Install).
void Register();

}  // namespace test_course

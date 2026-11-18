// =============================================================================
// native/frame_blend.h -- pictures IN BETWEEN two logic frames (feel test)
// =============================================================================
//
// WHY (notes/plans.md s.2, 2026-11-18): the game's logic is time-based but
// has quirks above 30 fps (the slope trip, findings/22; the moving platforms,
// ground grace...). The other way, the one Zelda 64: Recompiled takes: run
// the logic at a FIXED rate (logic_rate = 30, the original console's, or
// 60: fixed_step.h) and let the renderer draw the pictures in between at the
// screen's rate by BLENDING the last two recorded frames. This is the first
// version of that blend, to FEEL it: does 30 Hz logic + 144 Hz pictures
// play well?
//
// EXPERIMENTAL, OFF BY DEFAULT (frame_blending, decided 2026-11-18 after
// five feel-test rounds): matching ~400 draws between two recorded frames
// keeps meeting new cases in this game (mojo, springs, cameras). Fixed 30 Hz
// logic WITHOUT blending ("30 Plain") is the way forward, online too.
//
// HOW. The recorder (recorder.cpp) describes each game frame as a list of
// draws (frame.h). With two of them, A (older) and B (newer), a picture at
// blend t (0 = A, 1 = B) is B's list with:
//   * each draw's MATRICES (its model-view-projection, and in its material
//     block: world, normal rotation, camera position) taken from the line
//     between A's matching draw and B's;
//   * immediate geometry's VERTEX POSITIONS (Crash and the characters are
//     skinned by the game's CPU every frame, frame.h) and normals the same
//     way, when A's draw had the same number of vertices.
// Everything else (textures, colours, fades, the HUD's texts, which draws
// exist) is B's. A draw with no match in A is drawn as in B.
//
// MATCHING: draws are keyed by what stays the same for one object across
// frames (material, mesh buffers, vertex / index counts, layout, texture,
// target surface); the n-th draw with a key in B matches the n-th in A.
//
// SHADOW VOLUMES (stencil only, no colour) never pair: the game rebuilds them
// every frame, and a volume blended toward another frame's volume, or two
// pieces of one volume moved differently, broke the shadow (detached copy,
// streaks: first feel test). They take the blended camera and move with the
// nearest character (FOLLOWERS, below).
//
// NO BLEND ACROSS A CUT: a draw whose matrix changed by more than half its
// size (kSnapMatrix) is drawn as in B; when most of a frame's 3D draws do that
// (a camera cut, a teleport, a level change), the whole picture is B's. A
// vertex that moved more than kSnapUnits between A and B (a particle
// reborn elsewhere) is B's too.
//
// The in-between picture shows the world as it was 1 logic frame ago, at
// most: the price of blending is up to one logic frame of extra delay
// (33 ms at 30 Hz, 17 ms at 60).
// =============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "frame.h"

namespace native::frame_blend {

// What the matching found for one pair of frames (made once per new logic
// frame, used for every in-between picture of it).
struct Pairing {
  // Per draw of B: the index of its draw in A, or -1 (draw as in B).
  std::vector<int32_t> match;
  // FOLLOWERS: an immediate 3D draw with no pair (the shadow VOLUMES: the
  // game rebuilds them every frame with a different vertex count) moves
  // with the nearest blended immediate draw (Crash's body): at blend t its
  // vertices are shifted by (1 - t) x that body's movement from B back to A.
  // Else its shadow jumped in 30 Hz steps under a smoothly moving Crash
  // (flicker on and around him, first feel test). Per draw of B: the body's
  // move A - B, or no follow.
  struct Follow {
    bool on = false;
    float move[3] = {};
  };
  std::vector<Follow> follow;
  // THE OLDER MATRIX each draw blends from (feel test 5: far objects moved
  // and went back, or vanished for a picture):
  //  * ONE OBJECT, ONE BLEND: every draw with the same matrix in B (one
  //    object's passes; the world-space draws: the camera's) blends from one
  //    older matrix, the first paired one's partner. Passes of one object
  //    blended from different partners didn't line up (a depth-equal pass
  //    failed = gone for a picture).
  //  * UNPAIRED objects (a far object switching detail level, a copy coming
  //    into view: no partner) get the CAMERA's blend: B's matrix =
  //    camera_B x placement, so older = camera_A x camera_B^-1 x B's matrix.
  //    Drawn at B's matrix they sat up to a logic frame ahead of the blended
  //    world, then it caught up ("moving, then returning back").
  // Per draw of B; has_a_mvp[i] = 0: drawn with B's matrix.
  std::vector<std::array<float, 16>> a_mvp;
  std::vector<uint8_t> has_a_mvp;
  // THE CAMERA, TAKEN APART (feel test 8: the picture twitched when the
  // camera tilted up or down, far mojo too). Blending the camera's matrix
  // number by number makes things on screen move fast-slow-fast within
  // every logic step when the camera turns: a pulse 30 times a second. Now
  // the camera matrix (projection x view) is split into its LENS (scales,
  // centre, depth terms), its POSITION and its ROTATION; the picture's
  // camera = position on a line, rotation on the shortest arc, lens mixed.
  // Every 3D draw = that camera x its own placement (camera^-1 x its
  // matrix), placements blended number by number. cam_split = false: the
  // camera didn't come apart cleanly (not a plain perspective): old blend.
  struct CameraParts {
    double sx, sy, c0, c1, a, b, n;  // lens: row 0 = sx right + c0 forward, ...
    double eye[3];                    // position
    double q[4];                      // rotation (world -> view), x y z w
  };
  bool cam_split = false;
  CameraParts cam_pa{}, cam_pb{};
  std::vector<std::array<float, 32>> placement;  // per draw: A's then B's (column-major)
  std::vector<uint8_t> has_placement;
  // PARTICLES: their draws change size every frame (particles are born and
  // die), so they don't pair as draws; each PARTICLE (6 vertices: two
  // triangles) of an unpaired particle draw pairs with the nearest particle
  // of A's particle draws with the same texture instead (within
  // kParticleUnits). Else every spark and leaf moved in 30 Hz steps.
  struct QuadLink {
    uint32_t b_draw, b_quad, a_draw, a_quad;
  };
  std::vector<QuadLink> quads;
  // Most 3D draws jumped (a cut): every picture = B.
  bool cut = false;
  // Statistics for the log.
  uint32_t matched = 0, snapped = 0;
  // --debug_frame_blend_trace: why B's draws weren't blended (one line each).
  std::vector<std::string> notes;
};

// DEBUG CONSOLE `blendrec <seconds>` (console thread): with logic_rate on,
// turns blending on for `seconds` (if it was off), records every picture's
// 3D draws' screen points (debug_frame_blend_points, a temporary file),
// stops and closes it itself, puts blending back as it was, and returns a
// report: objects that VANISHED for one picture, or WENT BACK (moved one
// way, then reversed) for one, by kind (material, mesh, vertex/index count)
// with a sample screen spot. The file is deleted afterwards. (Made after a
// hand-run recording left its file open in a player's game, feel test 7.)
std::string RecordCommand(double seconds);

// Pairs B's draws with A's.
void Pair(const Frame& a, const Frame& b, Pairing& out);

// `out` = B with its matched draws' matrices and vertices moved to blend `t`
// (0 = A's, 1 = B's). `out`'s storage is reused.
void Blend(const Frame& a, const Frame& b, const Pairing& pairing, float t, Frame& out);

}  // namespace native::frame_blend

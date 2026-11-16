// =============================================================================
// tools/subtitle_editor/icons.h -- the editor's button symbols
// =============================================================================
//
// The symbols (play, stop, split, zoom...) are DRAWN with ImGui's shapes
// instead of taken from a font: the desktop's font may lack them (Linux
// fonts differ, Windows' Segoe UI has some, ImGui's own none), drawn ones
// look the same everywhere and scale with the text.
// =============================================================================
#pragma once

#include <imgui.h>

namespace subtitle_editor {

enum class Icon {
  kPlay,        // a triangle
  kPlayFromStart,  // |> : a bar, then a triangle
  kStop,        // a square
  kSplit,       // <|> : a cut with the halves moving apart
  kAdd,         // +
  kDelete,      // x
  kMerge,       // >|< : two halves coming together
  kZoomIn,      // a magnifier with +
  kZoomOut,     // a magnifier with -
  kFit,         // <-> between two bars
  kVolume,      // a speaker
  kHeight,      // a vertical double arrow
  kPreview,     // a screen
  kMenu,        // three dots
  kHelp,        // ?
  kCheck,       // a tick
  kUndo,        // an arrow turning back
  kRedo,        // an arrow turning forward (kUndo mirrored)
  kSave,        // a floppy disk
  kReload,      // a circular arrow
  kFilter,      // a funnel
  kSearch,      // a magnifier
  kHide,        // an eye struck through (unused)
  kFlag,        // a flag on a pole (needs reviewing)
  kNote,        // a page with lines (a note)
};

// Draws `icon` inside the square at `min` of side `size`.
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 min, float size, ImU32 colour);

// A button showing `icon` (and `label` after it, if any), `tip` on hover.
// `on` draws it in the "active" colour (a toggle that is on).
bool IconButton(const char* id, Icon icon, const char* tip, const char* label = nullptr, bool on = false);

// The icon alone in front of the next item (a field's label).
void IconLabel(Icon icon);

// Between two groups of a toolbar row: a thin vertical line, or, when the
// next group (`next_width` pixels) doesn't fit on this row, a new row.
void ToolbarGap(float next_width);

}  // namespace subtitle_editor

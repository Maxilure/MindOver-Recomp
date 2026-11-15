// =============================================================================
// tools/subtitle_editor/icons.cpp -- see icons.h
// =============================================================================
#include "icons.h"

#include <cmath>
#include <string>

namespace subtitle_editor {

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 min, float s, ImU32 c) {
  const float t = std::max(1.5f, s * 0.09f);  // line thickness
  const auto P = [&](float x, float y) { return ImVec2(min.x + x * s, min.y + y * s); };
  switch (icon) {
    case Icon::kPlay:
      dl->AddTriangleFilled(P(0.28f, 0.2f), P(0.28f, 0.8f), P(0.8f, 0.5f), c);
      break;
    case Icon::kPlayFromStart:
      dl->AddRectFilled(P(0.18f, 0.22f), P(0.28f, 0.78f), c);
      dl->AddTriangleFilled(P(0.36f, 0.2f), P(0.36f, 0.8f), P(0.84f, 0.5f), c);
      break;
    case Icon::kStop:
      dl->AddRectFilled(P(0.27f, 0.27f), P(0.73f, 0.73f), c, s * 0.05f);
      break;
    case Icon::kSplit:
      dl->AddLine(P(0.5f, 0.15f), P(0.5f, 0.85f), c, t);
      dl->AddTriangleFilled(P(0.38f, 0.35f), P(0.38f, 0.65f), P(0.12f, 0.5f), c);
      dl->AddTriangleFilled(P(0.62f, 0.35f), P(0.62f, 0.65f), P(0.88f, 0.5f), c);
      break;
    case Icon::kAdd:
      dl->AddLine(P(0.5f, 0.2f), P(0.5f, 0.8f), c, t * 1.2f);
      dl->AddLine(P(0.2f, 0.5f), P(0.8f, 0.5f), c, t * 1.2f);
      break;
    case Icon::kDelete:
      dl->AddLine(P(0.27f, 0.27f), P(0.73f, 0.73f), c, t * 1.2f);
      dl->AddLine(P(0.73f, 0.27f), P(0.27f, 0.73f), c, t * 1.2f);
      break;
    case Icon::kMerge:
      dl->AddLine(P(0.5f, 0.15f), P(0.5f, 0.85f), c, t);
      dl->AddTriangleFilled(P(0.12f, 0.35f), P(0.12f, 0.65f), P(0.4f, 0.5f), c);
      dl->AddTriangleFilled(P(0.88f, 0.35f), P(0.88f, 0.65f), P(0.6f, 0.5f), c);
      break;
    case Icon::kZoomIn:
    case Icon::kZoomOut:
    case Icon::kSearch:
      dl->AddCircle(P(0.42f, 0.42f), s * 0.24f, c, 20, t);
      dl->AddLine(P(0.6f, 0.6f), P(0.84f, 0.84f), c, t * 1.4f);
      if (icon != Icon::kSearch) dl->AddLine(P(0.31f, 0.42f), P(0.53f, 0.42f), c, t);
      if (icon == Icon::kZoomIn) dl->AddLine(P(0.42f, 0.31f), P(0.42f, 0.53f), c, t);
      break;
    case Icon::kFit:
      dl->AddLine(P(0.14f, 0.25f), P(0.14f, 0.75f), c, t);
      dl->AddLine(P(0.86f, 0.25f), P(0.86f, 0.75f), c, t);
      dl->AddLine(P(0.28f, 0.5f), P(0.72f, 0.5f), c, t);
      dl->AddTriangleFilled(P(0.22f, 0.5f), P(0.36f, 0.38f), P(0.36f, 0.62f), c);
      dl->AddTriangleFilled(P(0.78f, 0.5f), P(0.64f, 0.38f), P(0.64f, 0.62f), c);
      break;
    case Icon::kVolume:
      dl->AddRectFilled(P(0.14f, 0.38f), P(0.3f, 0.62f), c);
      dl->AddTriangleFilled(P(0.24f, 0.5f), P(0.52f, 0.2f), P(0.52f, 0.8f), c);
      dl->PathArcTo(P(0.52f, 0.5f), s * 0.17f, -0.9f, 0.9f, 8);
      dl->PathStroke(c, 0, t);
      dl->PathArcTo(P(0.52f, 0.5f), s * 0.31f, -0.9f, 0.9f, 10);
      dl->PathStroke(c, 0, t);
      break;
    case Icon::kHeight:
      dl->AddLine(P(0.5f, 0.28f), P(0.5f, 0.72f), c, t);
      dl->AddTriangleFilled(P(0.5f, 0.12f), P(0.36f, 0.32f), P(0.64f, 0.32f), c);
      dl->AddTriangleFilled(P(0.5f, 0.88f), P(0.36f, 0.68f), P(0.64f, 0.68f), c);
      break;
    case Icon::kPreview:
      dl->AddRect(P(0.12f, 0.22f), P(0.88f, 0.72f), c, s * 0.05f, 0, t);
      dl->AddLine(P(0.35f, 0.84f), P(0.65f, 0.84f), c, t);
      dl->AddTriangleFilled(P(0.43f, 0.35f), P(0.43f, 0.6f), P(0.62f, 0.47f), c);
      break;
    case Icon::kMenu:
      for (float x : {0.25f, 0.5f, 0.75f}) dl->AddCircleFilled(P(x, 0.5f), s * 0.075f, c);
      break;
    case Icon::kHelp: {
      const char* q = "?";
      const ImVec2 sz = ImGui::CalcTextSize(q);
      dl->AddCircle(P(0.5f, 0.5f), s * 0.36f, c, 20, t);
      dl->AddText(ImVec2(min.x + (s - sz.x) * 0.5f, min.y + (s - sz.y) * 0.5f), c, q);
      break;
    }
    case Icon::kCheck: {
      const ImVec2 pts[] = {P(0.2f, 0.52f), P(0.42f, 0.74f), P(0.82f, 0.28f)};
      dl->AddPolyline(pts, 3, c, 0, t * 1.4f);
      break;
    }
    case Icon::kUndo:
      dl->PathArcTo(P(0.52f, 0.56f), s * 0.26f, -3.6f, 0.9f, 16);
      dl->PathStroke(c, 0, t * 1.2f);
      dl->AddTriangleFilled(P(0.16f, 0.36f), P(0.38f, 0.26f), P(0.36f, 0.5f), c);
      break;
    case Icon::kRedo:  // kUndo mirrored left <-> right (angle a -> pi - a)
      dl->PathArcTo(P(0.48f, 0.56f), s * 0.26f, 2.2416f, 6.7416f, 16);
      dl->PathStroke(c, 0, t * 1.2f);
      dl->AddTriangleFilled(P(0.84f, 0.36f), P(0.62f, 0.26f), P(0.64f, 0.5f), c);
      break;
    case Icon::kHide:  // an eye (almond outline + pupil), struck through
      dl->PathArcTo(P(0.5f, 0.9f), s * 0.5f, -2.3f, -0.84f, 12);
      dl->PathArcTo(P(0.5f, 0.1f), s * 0.5f, 0.84f, 2.3f, 12);
      dl->PathStroke(c, ImDrawFlags_Closed, t);
      dl->AddCircleFilled(P(0.5f, 0.5f), s * 0.11f, c);
      dl->AddLine(P(0.18f, 0.82f), P(0.82f, 0.18f), c, t * 1.2f);
      break;
    case Icon::kSave:
      dl->AddRect(P(0.2f, 0.18f), P(0.8f, 0.82f), c, s * 0.04f, 0, t);
      dl->AddRectFilled(P(0.34f, 0.18f), P(0.66f, 0.38f), c);
      dl->AddRect(P(0.32f, 0.55f), P(0.68f, 0.82f), c, 0, 0, t);
      break;
    case Icon::kReload:
      dl->PathArcTo(P(0.5f, 0.5f), s * 0.28f, 0.4f, 5.4f, 18);
      dl->PathStroke(c, 0, t * 1.2f);
      dl->AddTriangleFilled(P(0.78f, 0.22f), P(0.82f, 0.48f), P(0.58f, 0.4f), c);
      break;
    case Icon::kFilter:
      dl->AddTriangleFilled(P(0.16f, 0.22f), P(0.84f, 0.22f), P(0.5f, 0.56f), c);
      dl->AddRectFilled(P(0.44f, 0.5f), P(0.56f, 0.82f), c);
      break;
  }
}

bool IconButton(const char* id, Icon icon, const char* tip, const char* label, bool on) {
  const float h = ImGui::GetFrameHeight();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float label_w = label ? ImGui::CalcTextSize(label).x + style.FramePadding.x : 0.0f;
  ImGui::PushID(id);
  if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  const bool pressed = ImGui::Button("##icon", ImVec2(h + label_w, h));
  if (on) ImGui::PopStyleColor();
  const ImVec2 min = ImGui::GetItemRectMin();
  // (Inside BeginDisabled the style alpha already dims it.)
  const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
  const float pad = h * 0.16f;
  DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(min.x + pad, min.y + pad), h - 2 * pad, colour);
  if (label) {
    ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + h - pad * 0.5f, min.y + style.FramePadding.y), colour, label);
  }
  if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
  ImGui::PopID();
  return pressed;
}

void IconLabel(Icon icon) {
  const float h = ImGui::GetFrameHeight();
  const ImVec2 min = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(h * 0.85f, h));
  const float pad = h * 0.18f;
  DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(min.x + pad * 0.5f, min.y + pad), h - 2 * pad,
           ImGui::GetColorU32(ImGuiCol_TextDisabled));
  ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
}

void ToolbarGap(float next_width) {
  const float h = ImGui::GetFrameHeight();
  const float gap = ImGui::GetFontSize() * 0.7f;
  const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
  ImGui::SameLine(0, gap);
  if (ImGui::GetCursorScreenPos().x + 1 + gap + next_width > right) {
    ImGui::NewLine();  // no room: the group starts the next row
    return;
  }
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(1, h));
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h * 0.15f), ImVec2(p.x, p.y + h * 0.85f),
                                      ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
  ImGui::SameLine(0, gap);
}

}  // namespace subtitle_editor

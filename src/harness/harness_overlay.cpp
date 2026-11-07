#include "harness/harness_overlay.h"

#include <cstdio>

#include <imgui.h>

#include "harness/harness.h"

namespace skate3::harness {

bool HarnessOverlay::WantsContinuousRepaint() const {
  const Status st = CurrentStatus();
  return st.recording || st.playing || st.scripted || st.paused;
}

void HarnessOverlay::OnDraw(ImGuiIO& io) {
  (void)io;
  const Status st = CurrentStatus();
  if (!st.enabled || !(st.recording || st.playing || st.scripted || st.paused)) {
    return;
  }
  char label[64];
  ImVec4 color;
  if (st.recording) {
    std::snprintf(label, sizeof(label), "REC %.1fs", st.record_seconds);
    color = ImVec4(1.0f, 0.25f, 0.25f, 1.0f);
  } else if (st.playing) {
    std::snprintf(label, sizeof(label), "PLAY");
    color = ImVec4(0.35f, 1.0f, 0.45f, 1.0f);
  } else if (st.paused) {
    std::snprintf(label, sizeof(label), "PAUSED");
    color = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
  } else {
    std::snprintf(label, sizeof(label), "INPUT");
    color = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);
  }
  ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.45f);
  ImGui::Begin("##harness_overlay", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::TextColored(color, "%s", label);
  ImGui::End();
}

}  // namespace skate3::harness

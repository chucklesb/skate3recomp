#pragma once
// Scenario harness on-screen indicator (ASCII: the UI font may lack
// symbol glyphs): "REC <s>" while recording input,
// "PLAY" during playback, "INPUT" while a scripted input step holds the pad,
// "PAUSED" while frame-stepping. Drawn by the host UI over the guest output,
// so harness screenshots (guest output only) never contain it.

#include <rex/ui/imgui_dialog.h>

namespace skate3::harness {

class HarnessOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit HarnessOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  // Repaint continuously only while something is shown (the REC timer).
  bool WantsContinuousRepaint() const override;
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace skate3::harness

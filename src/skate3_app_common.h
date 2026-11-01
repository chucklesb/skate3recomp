#pragma once

#include "generated/skate3_init.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rex/rex_app.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

#include "skate3_native_debug_dialog.h"

namespace rex::ui {
class ImGuiDrawer;
}

class Skate3BaseApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  ~Skate3BaseApp() override;

 protected:
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override;
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnPostSetup() override;
  void OnShutdown() override;

 private:
  void InstallRecipeOverlay();
  void InstallBigDeviceAliases();
  void InstallDlcPackages();
  void ToggleSimpleSettings();
  void ToggleNativeDebug();
  void ApplySettingsCursorMode();
  void ApplyGameplayCursorMode();
  void RestartGame();
  // The one clean exit (the overlay's Close Game and the Options screen's
  // Quit Game): closes the window like its X button. Callable from any thread.
  void CloseGame();
  void SaveDrawFingerprintLog();
  void LogUserMarker();
  void LogDebugMarker();
  void ApplySelectedProfileToRuntime();
  // rex::ui::RegisterBind plus bookkeeping: OnShutdown unregisters every bind
  // registered through this, newest first (the SDK has no RAII bind handle).
  void RegisterAppBind(std::string_view name, std::string_view default_key,
                       std::string_view description, std::function<void()> callback);
  void UnregisterAppBinds();

  static bool IsRecipeNameChar(char c);
  static std::set<std::string> DiscoverRecipeAliases(
      const std::filesystem::path& content_root);
  static bool CreateOverlayDirectory(const std::filesystem::path& overlay_root,
                                     std::string_view guest_path);

  std::filesystem::path config_path_;
  std::filesystem::path user_settings_path_;
  std::filesystem::path profiles_path_;
  std::unique_ptr<rex::ui::SimpleSettingsDialog> simple_settings_dialog_;
  std::unique_ptr<skate3::NativeDebugDialog> native_debug_dialog_;
  std::unique_ptr<skate3::RenderModeIndicator> render_mode_indicator_;
  bool recipe_overlay_installed_ = false;
  bool big_device_aliases_installed_ = false;
  std::atomic<uint32_t> debug_marker_count_{0};
  // Names of the binds registered by RegisterAppBind, in registration order.
  std::vector<std::string> registered_binds_;
};

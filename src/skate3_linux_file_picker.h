// Linux file picker shared by the ISO and title update installers.
//
// Compiled only on non-Windows, non-Apple builds (see CMakeLists.txt).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace skate3 {

struct LinuxFilePickerRequest {
  // Dialog title.
  std::string title;
  // Name of the first filter (for example "Xbox 360 ISO (*.iso)") and its glob
  // patterns. Leave the patterns empty to offer only "All files".
  std::string filter_name;
  std::vector<std::string> filter_patterns;
  // Environment variable that lets the user skip the dialog entirely; only
  // used in the message logged when no file chooser can be shown.
  std::string env_var;
};

// Shows a modal "open file" dialog and returns the chosen path, or an empty
// path if the user cancelled or no dialog could be shown.
//
// Must be called on the main (UI) thread. GTK is initialised lazily on the
// first call; if GTK cannot open a display, falls back to `zenity`, then
// `kdialog` (spawned without a shell), and finally logs how to pass the file
// without a dialog.
std::filesystem::path PickFileLinux(const LinuxFilePickerRequest& request);

}  // namespace skate3

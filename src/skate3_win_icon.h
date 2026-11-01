#pragma once

#include <filesystem>

namespace skate3 {

// Application icon sourced from the USER'S OWN game files at runtime; the
// shipped executable and the repository deliberately contain no EA artwork
// (same clean-distribution rule as every other game asset).
//
// Windows only (no-op elsewhere): the 64x64 title tile PNG embedded in
// game/nxeart (the Xbox dashboard-art package) becomes the window, taskbar
// and Alt-Tab icon via WM_SETICON, every run.
//
// The exe file itself is never modified. Upstream used to self-patch an
// Explorer icon into skate3.exe on disk (rename the running image aside,
// swap in a patched copy, delete the original via a hidden PowerShell);
// antivirus heuristics flag that pattern as a trojan dropper, and it would
// break an Authenticode signature, so it was removed.
//
// Runs on a background thread; failures only log (the icon stays default).
void ApplyGameIconFromGameData(const std::filesystem::path& game_data_root,
                               void* native_window_handle);

}  // namespace skate3

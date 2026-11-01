#include "skate3_options_menu.h"

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/overlay/simple_settings_restart.h>

REXCVAR_DEFINE_BOOL(skate3_options_menu_entry, true, "Skate 3",
                    "Add the recomp settings to the game's Options menu (takes effect on "
                    "restart)");

namespace skate3::options_menu {

namespace {
std::function<void()> g_open_settings;
std::function<void()> g_save_settings;
std::function<void()> g_restart;
std::function<void()> g_quit;
}  // namespace

#if SKATE3_HAS_TITLE_UPDATE
namespace {

// The game's Options screen (FrontEndState_Options, options.apt) is
// table-driven (Title Update 3 image):
// - kOptionTable: 36 x {char* title string ID, char* widget type}, indexed by
//   option id; the type is "option" (submenu/action), "slider" or "selector".
// - kScreenLayouts: int32[13][16] option ids per screen, 0-terminated. Screens
//   0 and 1 are the root page (Difficulty, Audio, Video, Controls, Online,
//   Skate Feed) with ten free slots each.
// - The state keeps the current screen at +68 and the highlighted row at +72.
//   The movie pulls everything through GameSettings_* APT natives and the
//   state's per-row getters; Back (input handler, event 3) always returns to
//   the root screen.
// Option ids that no layout uses are borrowed for our root rows (titles
// repointed at our string IDs, types at "option"). Ids the game's handlers
// still special-case (24, a selector) get those paths answered for our row. Our
// pages are screen ids past the game's tables (kFirstPageScreen + page index)
// that every table reader below answers for from host rows. All text goes
// through the localizer hook, which resolves our host-owned string IDs.
constexpr uint32_t kOptionTable = 0x83026EC8;
constexpr uint32_t kScreenLayouts = 0x83026FE8;
constexpr uint32_t kScreenCount = 13;
constexpr uint32_t kSlotsPerScreen = 16;
constexpr uint32_t kRootScreens[] = {0, 1};
constexpr int32_t kRootLayout[] = {2, 3, 4, 5, 6, 35};

struct SpareOption {
  int32_t id;
  std::string_view title;  // the game's (unused) title string ID
  std::string_view type;
};
constexpr SpareOption kSpareOptions[] = {
    {23, "0", "option"},
    {1, "ID_GAMESETTINGS_MUSICPLAYER", "option"},
    {22, "ID_GAMESETTINGS_VIEW_TOS", "option"},
    {27, "ID_GAMESETTINGS_RESET_CAREER", "option"},
    {18, "ID_GAMESETTINGS_VSYNC_TOGGLE", "selector"},
    {20, "ID_GAMESETTINGS_SIXAXIS_TOGGLE", "selector"},
    // Still a live selector in the input handler (left/right toggle the
    // distance units) and the value getter; both are answered for our row.
    {24, "ID_GAMESETTINGS_UNITS_TOGGLE", "selector"},
};
constexpr int32_t kAudioOptionId = 3;      // layout sanity check
constexpr int32_t kOptionTypeId = 23;      // its type pointer: "option"
constexpr int32_t kSelectorOptionId = 11;  // camera angle: "selector"
constexpr uint32_t kFirstPageScreen = kScreenCount;

constexpr uint32_t kStateScreenOffset = 68;
constexpr uint32_t kStateRowOffset = 72;

// Literal ("#") string ID the GameSettings natives push for "nothing".
constexpr uint32_t kStringEmpty = 0x822039C4;

// Input handler events (FrontEndState_Options vtable +40).
constexpr uint32_t kInputBack = 3;
constexpr uint32_t kInputLeft = 14;
constexpr uint32_t kInputRight = 15;

// Refresh modes passed to the movie wrapper: full screen rebuild vs values.
constexpr uint32_t kRefreshScreen = 1;
constexpr uint32_t kRefreshValues = 50;

// FE sounds the Options screen uses for sub-screen entry, Back and selectors.
constexpr uint64_t kSubmenuSound = 0x373D38D7EA167A71ull;
constexpr uint64_t kBackSound = 0x4D927CD64CC8B818ull;
constexpr uint64_t kSelectorLeftSound = 0xAD8BF9A0B4997B49ull;
constexpr uint64_t kSelectorRightSound = 0xD8EE38A61C45EA28ull;

// FE popups (as FrontEndState_Options builds its confirmations): a descriptor
// initialized by sub_82D0B0B0 holds the title and message string IDs, the
// button count and labels, and the guest callback the chosen button index is
// passed to (r4); sub_82D0A8D0 shows it on the popup manager.
constexpr uint32_t kPopupManagerPtr = 0x830CFE1C;
constexpr uint32_t kPopupDescSize = 1536;
constexpr uint32_t kPopupTitleOffset = 0;      // char[128]
constexpr uint32_t kPopupMessageOffset = 128;  // char[256]
constexpr uint32_t kPopupButtonCountOffset = 384;
constexpr uint32_t kPopupButtonOffsets[] = {388, 516};  // char[128] each
constexpr uint32_t kPopupCallbackOffset = 1412;
// The manager keeps the shown popups (its own 1452-byte copies of the
// descriptors) in a vector of pointers at +112 (begin) / +116 (end); each copy
// carries the id sub_82D0A8D0 returned at +1428. Every path that answers a
// popup passes that id to its callback in r3: a button press (sub_825EF698,
// r4 = button index) and the manager flushing its popups (sub_82D0A738,
// r4 = -2). Ids come from an ever-increasing counter, so a stale one never
// matches a later popup. A screen can also close a popup by id without any
// callback (sub_82D0A628).
constexpr uint32_t kPopupQueueBeginOffset = 112;
constexpr uint32_t kPopupQueueEndOffset = 116;
constexpr uint32_t kPopupIdOffset = 1428;
constexpr uint32_t kPopupQueueMax = 64;  // sanity bound for the queue walk
constexpr int32_t kNoPopup = -1;         // the descriptor's own "no id" value
// The Options screen's own Video Calibration popup callback; hooked below and
// answered for our popup while it is open.
constexpr uint32_t kPopupCallback = 0x8260EA48;

// A selector row: left/right cycle its labels. Restart rows stage the choice
// (shown, not applied) until the user restarts from the Back prompt.
struct Row {
  std::string title;
  std::string desc;
  std::vector<std::string> labels;
  std::function<int()> get;        // label index the cvars select
  std::function<void(int)> set;    // write the cvars for a label index
  std::function<bool()> enabled;   // null = always enabled
  bool rebuild_on_change = false;  // other rows' enabled state depends on it
  bool restart = false;
  std::optional<int> pending;      // restart rows: staged label index
  // Guest string IDs, resolved by the localizer hook.
  uint32_t title_id = 0;
  uint32_t desc_id = 0;
  std::vector<uint32_t> label_ids;
};

struct Page {
  std::string title;
  std::vector<Row> rows;
  uint32_t title_id = 0;
};

// A row we add to the root Options screen: opens a page or the overlay, or
// asks to quit the game.
constexpr int kOverlayPage = -1;  // RootRow::page: the host settings overlay
constexpr int kQuitPage = -2;     // RootRow::page: the Quit Game prompt
struct RootRow {
  std::string title;
  int32_t option_id = 0;
  int page = kOverlayPage;  // a g_pages index, kOverlayPage or kQuitPage
  uint32_t title_id = 0;
};

std::atomic<bool> g_installed{false};
std::vector<Page> g_pages;
std::vector<RootRow> g_root_rows;
std::unordered_map<uint32_t, uint32_t> g_locale;  // ID hash -> guest text
uint32_t g_type_selector = 0;
uint32_t g_type_option = 0;
uint32_t g_popup_desc = 0;
// Id of the last restart prompt we showed, or kNoPopup. Only an identity: the
// popup manager's queue says whether that popup is still open, so no path
// that closes it (a button, a flush, a screen closing it by id) can leave a
// stale "open" state behind.
int32_t g_restart_popup_id = kNoPopup;  // guest thread only
// Same for the Quit Game prompt.
int32_t g_quit_popup_id = kNoPopup;  // guest thread only

// A two-button popup's string IDs (copied into the popup descriptor).
struct Prompt {
  std::string_view name;  // for the log
  std::string_view title;
  std::string_view message;
  std::string_view buttons[2];
};
constexpr Prompt kRestartPrompt = {"restart",
                                   "ID_RECOMP_RESTART_TITLE",
                                   "ID_RECOMP_RESTART_MESSAGE",
                                   {"ID_RECOMP_RESTART_NOW", "ID_RECOMP_RESTART_DISCARD"}};
// The buttons are the game's own localized Cancel / Quit, in the order of its
// own quit prompts (No before Yes), so A on the default button cancels. This
// popup style shows the message, not the title, so the message carries the
// question.
constexpr Prompt kQuitPrompt = {"quit",
                                "ID_RECOMP_QUIT_TITLE",
                                "ID_RECOMP_QUIT_MESSAGE",
                                {"ID_COMMON_CANCEL", "ID_COMMON_QUIT"}};
constexpr int32_t kQuitButton = 1;

// Same hash GetLocaleText (sub_825AA818) feeds the localizer.
uint32_t LocaleHash(std::string_view id) {
  uint32_t hash = 0xFFFFFFFFu;
  for (char c : id) {
    hash = hash * 33 + static_cast<uint8_t>(c);
  }
  return hash;
}

bool HasCvar(std::string_view name) { return rex::cvar::GetFlagInfo(name) != nullptr; }

// ---- Rows -------------------------------------------------------------------

int NearestIndex(const std::vector<double>& values, double value) {
  int best = 0;
  for (int i = 1; i < static_cast<int>(values.size()); ++i) {
    if (std::abs(values[i] - value) < std::abs(values[best] - value)) {
      best = i;
    }
  }
  return best;
}

int ShownIndex(const Row& row) { return row.pending.value_or(row.get()); }

// The shown label, or null for a row without labels or whose getter answers
// out of range (the table getters then show nothing instead of indexing).
std::optional<int> ShownLabel(const Row& row) {
  const int index = ShownIndex(row);
  if (index < 0 || index >= static_cast<int>(row.labels.size())) {
    return std::nullopt;
  }
  return index;
}

bool HasPendingRestart() {
  for (const Page& page : g_pages) {
    for (const Row& row : page.rows) {
      if (row.pending) {
        return true;
      }
    }
  }
  return false;
}

// Rows for cvars this build lacks are skipped; the Add* helpers return null.
Row* AddRow(Page& page, std::string title, std::string desc) {
  Row row;
  row.title = std::move(title);
  row.desc = std::move(desc);
  page.rows.push_back(std::move(row));
  return &page.rows.back();
}

Row* AddToggle(Page& page, std::string title, std::string desc, std::vector<std::string> cvars) {
  std::erase_if(cvars, [](const std::string& name) { return !HasCvar(name); });
  if (cvars.empty()) {
    return nullptr;
  }
  Row* row = AddRow(page, std::move(title), std::move(desc));
  row->labels = {"Off", "On"};
  row->get = [cvars] { return rex::cvar::Query<bool>(cvars.front()) ? 1 : 0; };
  row->set = [cvars](int value) {
    for (const auto& name : cvars) {
      rex::cvar::SetFlagByName(name, value ? "true" : "false");
    }
  };
  return row;
}

Row* AddNumberChoice(Page& page, std::string title, std::string desc,
                     std::vector<std::string> labels, std::vector<double> values,
                     std::vector<std::string> cvars) {
  std::erase_if(cvars, [](const std::string& name) { return !HasCvar(name); });
  if (cvars.empty()) {
    return nullptr;
  }
  Row* row = AddRow(page, std::move(title), std::move(desc));
  row->labels = std::move(labels);
  row->get = [values, cvars] {
    return NearestIndex(values, rex::cvar::Query<double>(cvars.front()));
  };
  row->set = [values, cvars](int index) {
    if (index < 0 || index >= static_cast<int>(values.size())) {
      return;
    }
    const std::string text = std::to_string(values[index]);
    for (const auto& name : cvars) {
      rex::cvar::SetFlagByName(name, text);
    }
  };
  return row;
}

Row* AddStringChoice(Page& page, std::string title, std::string desc,
                     std::vector<std::string> labels, std::vector<std::string> values,
                     std::string cvar) {
  if (!HasCvar(cvar)) {
    return nullptr;
  }
  Row* row = AddRow(page, std::move(title), std::move(desc));
  row->labels = std::move(labels);
  row->get = [values, cvar] {
    const std::string current = rex::cvar::Query<std::string>(cvar);
    const auto it = std::find(values.begin(), values.end(), current);
    return it != values.end() ? static_cast<int>(it - values.begin()) : 0;
  };
  row->set = [values, cvar](int index) {
    if (index >= 0 && index < static_cast<int>(values.size())) {
      rex::cvar::SetFlagByName(cvar, values[index]);
    }
  };
  return row;
}

// The overlay's restart-applied settings (SDK), keyed by id.
using RestartSettings = std::unordered_map<std::string, rex::ui::RestartSetting>;

Row* AddRestart(Page& page, const RestartSettings& settings, const std::string& id) {
  const auto it = settings.find(id);
  if (it == settings.end()) {
    return nullptr;
  }
  const rex::ui::RestartSetting& setting = it->second;
  Row* row = AddRow(page, setting.label, setting.desc + " Takes effect after a restart.");
  row->labels = setting.options;
  row->get = setting.current;
  row->set = setting.apply;
  row->restart = true;
  return row;
}

bool NativeRendererActive() {
  return !HasCvar("skate3_native_render_scene") ||
         rex::cvar::Query<bool>("skate3_native_render_scene");
}

// Native-renderer-only rows grey out while the emulated renderer is active.
void NativeOnly(Row* row) {
  if (row) {
    row->enabled = NativeRendererActive;
  }
}

Page BuildDisplayPage(const RestartSettings& settings) {
  Page page{"Display"};
  for (const char* id : {"graphics_api", "graphics_device", "render_scale", "frame_cap",
                         "aspect_ratio", "fullscreen", "monitor", "vsync", "tearing"}) {
    AddRestart(page, settings, id);
  }
  return page;
}

Page BuildGraphicsPage(const RestartSettings& settings) {
  Page page{"Graphics"};
  // The renderer switch only exists on builds that can run the native path.
  if (HasCvar("skate3_native_render") && rex::cvar::Query<bool>("skate3_native_render")) {
    if (Row* row = AddToggle(page, "Renderer",
                             "Native renders the game directly on your GPU (much faster); "
                             "Emulated replays the console GPU exactly.",
                             {"skate3_native_render_scene"})) {
      row->labels = {"Emulated", "Native"};
      row->rebuild_on_change = true;
    }
  }
  NativeOnly(AddRestart(page, settings, "msaa"));
  NativeOnly(AddRestart(page, settings, "shadow_quality"));
  NativeOnly(AddToggle(page, "Ambient Occlusion",
                       "Soft contact shading where surfaces meet: under ledges, rails and "
                       "the skater.",
                       {"skate3_native_render_scene_ssao"}));
  NativeOnly(AddToggle(page, "Bloom", "Glow around bright light sources, strongest at night.",
                       {"skate3_native_render_scene_bloom"}));
  NativeOnly(AddToggle(page, "Volumetric Lighting",
                       "Sun shafts and directional haze that follow the time of day.",
                       {"skate3_native_render_scene_shafts", "skate3_native_render_scene_haze"}));
  return page;
}

Page BuildViewPage() {
  Page page{"View"};
  {
    std::vector<std::string> labels;
    std::vector<double> values;
    for (int degrees = 40; degrees <= 120; degrees += 5) {
      labels.push_back(degrees == 60 ? "60 (Default)" : std::to_string(degrees));
      values.push_back(degrees);
    }
    AddNumberChoice(page, "Field of View", "Gameplay camera field of view in degrees.",
                    std::move(labels), std::move(values), {"skate3_field_of_view"});
  }
  AddNumberChoice(page, "Draw Distance",
                  "How far world detail and characters stay at full detail.",
                  {"Original", "2x", "3x", "5x"}, {1.0, 2.0, 3.0, 5.0},
                  {"skate3_draw_distance_scale", "skate3_lod_distance_scale"});
  AddNumberChoice(page, "World Streaming",
                  "Pre-loads neighbouring areas within this radius so they are ready "
                  "before you reach them.",
                  {"Original", "Near (100 m)", "Extended (200 m)", "Far (300 m)"},
                  {0.0, 100.0, 200.0, 300.0}, {"skate3_draw_distance_stream_probe"});
  AddToggle(page, "FPS Counter", "Shows the frame rate in the corner of the screen.",
            {"show_fps_counter"});
  AddToggle(page, "Renderer Indicator",
            "Corner readout of which renderer drew the last frame (Native or Emulated).",
            {"skate3_native_render_mode_indicator"});
  return page;
}

Page BuildControlsPage(const RestartSettings& settings) {
  Page page{"PC Controls"};
  AddRestart(page, settings, "mnk_mode");
  AddRestart(page, settings, "mnk_capture_mouse");
  {
    const std::vector<double> values = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5,
                                        2.0,  2.5, 3.0,  4.0, 5.0};
    std::vector<std::string> labels;
    for (double value : values) {
      char text[16];
      std::snprintf(text, sizeof(text), "%.2f", value);
      labels.push_back(value == 1.0 ? std::string(text) + " (Default)" : std::string(text));
    }
    AddNumberChoice(page, "Mouse Sensitivity",
                    "Mouse-look speed while Mouse & Keyboard Mode is active.", std::move(labels),
                    values, {"mnk_sensitivity"});
  }
  AddRestart(page, settings, "input_backend");
  AddStringChoice(page, "Settings Menu Shortcut",
                  "Controller button combination that opens the full PC settings.",
                  {"RB + Start", "LB + RB + Start", "Back + Start", "L3 + R3"},
                  {"rb+start", "lb+rb+start", "back+start", "l3+r3"}, "menu_chord");
  return page;
}

Page BuildSystemPage(const RestartSettings& settings) {
  Page page{"System"};
  AddToggle(page, "Mute", "Silences all game audio.", {"audio_mute"});
  AddRestart(page, settings, "audio_buffer");
  AddRestart(page, settings, "language");
  return page;
}

// Pages with rows each get a root row (in spare-id order), then the overlay.
void BuildMenu() {
  RestartSettings settings;
  for (auto& setting : rex::ui::GetRestartSettings()) {
    std::string id = setting.id;
    settings.emplace(std::move(id), std::move(setting));
  }
  for (Page page : {BuildDisplayPage(settings), BuildGraphicsPage(settings), BuildViewPage(),
                    BuildControlsPage(settings), BuildSystemPage(settings)}) {
    if (page.rows.empty()) {
      continue;
    }
    g_root_rows.push_back({page.title, 0, static_cast<int>(g_pages.size())});
    g_pages.push_back(std::move(page));
  }
  g_root_rows.push_back({"All PC Settings", 0, kOverlayPage});
  g_root_rows.push_back({"Quit Game", 0, kQuitPage});
  static_assert(std::size(kSpareOptions) >= 7, "one spare option id per root row");
  for (size_t i = 0; i < g_root_rows.size(); ++i) {
    g_root_rows[i].option_id = kSpareOptions[i].id;
  }
}

// ---- Guest strings ------------------------------------------------------------

// Writes every string ID and its display text into one guest block and fills
// the localizer map. Returns false when the allocation fails.
bool PublishStrings(rex::memory::Memory* memory, uint8_t* base) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (size_t i = 0; i < g_root_rows.size(); ++i) {
    entries.emplace_back("ID_RECOMP_ROOT_" + std::to_string(i), g_root_rows[i].title);
  }
  for (size_t p = 0; p < g_pages.size(); ++p) {
    const Page& page = g_pages[p];
    const std::string prefix = "ID_RECOMP_PAGE_" + std::to_string(p);
    entries.emplace_back(prefix, page.title);
    for (size_t r = 0; r < page.rows.size(); ++r) {
      const Row& row = page.rows[r];
      const std::string row_prefix = prefix + "_R" + std::to_string(r);
      entries.emplace_back(row_prefix, row.title);
      entries.emplace_back(row_prefix + "_DESC", row.desc);
      for (size_t v = 0; v < row.labels.size(); ++v) {
        entries.emplace_back(row_prefix + "_V" + std::to_string(v), row.labels[v]);
      }
    }
  }
  // Popup strings: only their localized text is needed (the descriptor holds
  // copies of the IDs).
  const std::pair<std::string_view, std::string_view> popup_text[] = {
      {kRestartPrompt.title, "Restart Required"},
      {kRestartPrompt.message,
       "Some changes take effect after Skate 3 restarts. Restart now to apply them?"},
      {kRestartPrompt.buttons[0], "Restart Now"},
      {kRestartPrompt.buttons[1], "Discard Changes"},
      {kQuitPrompt.title, "Quit Skate 3?"},
      {kQuitPrompt.message, "Quit Skate 3? Progress since the last save will be lost."},
  };
  for (const auto& [id, text] : popup_text) {
    entries.emplace_back(std::string(id), std::string(text));
  }

  uint32_t size = 0;
  for (const auto& [id, text] : entries) {
    size += static_cast<uint32_t>(id.size() + text.size() + 2);
  }
  const uint32_t block = memory->SystemHeapAlloc(size);
  g_popup_desc = memory->SystemHeapAlloc(kPopupDescSize);
  if (!block || !g_popup_desc) {
    return false;
  }
  uint32_t cursor = block;
  auto write = [&](const std::string& text) {
    const uint32_t address = cursor;
    std::memcpy(base + cursor, text.data(), text.size());
    base[cursor + text.size()] = 0;
    cursor += static_cast<uint32_t>(text.size() + 1);
    return address;
  };
  std::vector<uint32_t> ids;
  for (const auto& [id, text] : entries) {
    ids.push_back(write(id));
    g_locale[LocaleHash(id)] = write(text);
  }
  size_t next = 0;
  for (auto& root : g_root_rows) {
    root.title_id = ids[next++];
  }
  for (auto& page : g_pages) {
    page.title_id = ids[next++];
    for (auto& row : page.rows) {
      row.title_id = ids[next++];
      row.desc_id = ids[next++];
      for (size_t v = 0; v < row.labels.size(); ++v) {
        row.label_ids.push_back(ids[next++]);
      }
    }
  }
  return true;
}

// ---- Tables -------------------------------------------------------------------

bool GuestStringEquals(uint8_t* base, uint32_t address, std::string_view expected) {
  if (address < 0x82000000u || address >= 0x84000000u) {
    return false;
  }
  const char* text = reinterpret_cast<const char*>(base + address);
  return std::strncmp(text, expected.data(), expected.size()) == 0 &&
         text[expected.size()] == '\0';
}

uint32_t LayoutSlot(uint32_t screen, uint32_t slot) {
  return kScreenLayouts + (screen * kSlotsPerScreen + slot) * 4;
}

uint32_t OptionTitleSlot(int32_t id) { return kOptionTable + id * 8; }
uint32_t OptionTypeSlot(int32_t id) { return kOptionTable + id * 8 + 4; }

bool ImageMatches(uint8_t* base) {
  if (!GuestStringEquals(base, REX_LOAD_U32(OptionTypeSlot(kSelectorOptionId)), "selector") ||
      !GuestStringEquals(base, REX_LOAD_U32(OptionTypeSlot(kOptionTypeId)), "option") ||
      !GuestStringEquals(base, REX_LOAD_U32(OptionTitleSlot(kAudioOptionId)),
                         "ID_GAMESETTINGS_AUDIO_SETTINGS") ||
      !GuestStringEquals(base, kStringEmpty, "#")) {
    return false;
  }
  for (const SpareOption& spare : kSpareOptions) {
    if (!GuestStringEquals(base, REX_LOAD_U32(OptionTitleSlot(spare.id)), spare.title) ||
        !GuestStringEquals(base, REX_LOAD_U32(OptionTypeSlot(spare.id)), spare.type)) {
      return false;
    }
    for (uint32_t slot = 0; slot < kScreenCount * kSlotsPerScreen; ++slot) {
      if (static_cast<int32_t>(REX_LOAD_U32(kScreenLayouts + slot * 4)) == spare.id) {
        return false;
      }
    }
  }
  for (uint32_t screen : kRootScreens) {
    for (uint32_t slot = 0; slot < kSlotsPerScreen; ++slot) {
      const int32_t expected = slot < std::size(kRootLayout) ? kRootLayout[slot] : 0;
      if (static_cast<int32_t>(REX_LOAD_U32(LayoutSlot(screen, slot))) != expected) {
        return false;
      }
    }
  }
  return true;
}

// ---- Guest helpers ------------------------------------------------------------

bool IsInstalled() { return g_installed.load(std::memory_order_acquire); }

// The page shown by an Options state, or null on the game's own screens.
Page* PageOf(uint8_t* base, uint32_t state) {
  if (state < 0x10000) {
    return nullptr;
  }
  const uint32_t screen = REX_LOAD_U32(state + kStateScreenOffset);
  if (screen < kFirstPageScreen || screen - kFirstPageScreen >= g_pages.size()) {
    return nullptr;
  }
  return &g_pages[screen - kFirstPageScreen];
}

Row* RowAt(Page& page, uint32_t row) {
  return row < page.rows.size() ? &page.rows[row] : nullptr;
}

bool RowEnabled(const Row& row) { return !row.enabled || row.enabled(); }

// Our row at `row` of a game screen, or null.
const RootRow* RootRowAt(uint8_t* base, uint32_t state, uint32_t row) {
  if (state < 0x10000 || row >= kSlotsPerScreen) {
    return nullptr;
  }
  const uint32_t screen = REX_LOAD_U32(state + kStateScreenOffset);
  if (screen >= kScreenCount) {
    return nullptr;
  }
  const int32_t id = static_cast<int32_t>(REX_LOAD_U32(LayoutSlot(screen, row)));
  for (const RootRow& root : g_root_rows) {
    if (root.option_id == id) {
      return &root;
    }
  }
  return nullptr;
}

// The live FrontEndState_Options instance, as the GameSettings natives find it.
uint32_t OptionsState(PPCContext& ctx, uint8_t* base) {
  sub_824AD240(ctx, base);
  const uint32_t manager = ctx.r3.u32;
  if (manager < 0x10000) {
    return 0;
  }
  const uint32_t owner = REX_LOAD_U32(manager + 8);
  return owner >= 0x10000 ? REX_LOAD_U32(owner + 180) : 0;
}

int32_t AptArgInt(PPCContext& ctx, uint8_t* base, uint32_t index) {
  ctx.r3.u64 = index;
  sub_82E62E00(ctx, base);
  sub_82E5F2A8(ctx, base);
  return ctx.r3.s32;
}

void AptReturnInt(PPCContext& ctx, uint8_t* base, int32_t value) {
  ctx.r3.s64 = value;
  sub_82E86550(ctx, base);
}

void AptReturnString(PPCContext& ctx, uint8_t* base, uint32_t text) {
  ctx.r3.u64 = text;
  sub_82E864F8(ctx, base);
}

void PlaySound(PPCContext& ctx, uint8_t* base, uint64_t sound) {
  ctx.r3.u64 = sound;
  ctx.r4.u64 = 0;
  sub_825DFAF0(ctx, base);
}

// What the game does after changing screen or a value: clear the pending
// flag and have the movie wrapper (state+56, vtable +20) rebuild.
void RefreshScreen(PPCContext& ctx, uint8_t* base, uint32_t state, uint32_t mode) {
  REX_STORE_U8(state + 49, 0);
  const uint32_t movie = REX_LOAD_U32(state + 56);
  const uint32_t function = REX_LOAD_U32(REX_LOAD_U32(movie) + 20);
  ctx.r3.u64 = movie;
  ctx.r4.u64 = REX_LOAD_U32(state + 8);
  ctx.r5.u64 = mode;
  REX_CALL_INDIRECT_FUNC(function);
}

// The label `delta` steps from `shown`, wrapping around; -1 for no labels.
int SteppedIndex(int shown, int delta, int count) {
  if (count <= 0) {
    return -1;
  }
  const int64_t wrapped = (static_cast<int64_t>(shown) + delta) % count;
  return static_cast<int>(wrapped < 0 ? wrapped + count : wrapped);
}

// Live rows apply and save immediately; restart rows only stage the choice.
// A row without labels has nothing to step to: no sound, no change.
bool StepRow(PPCContext& ctx, uint8_t* base, uint32_t state, Row& row, int delta) {
  const int index = SteppedIndex(ShownIndex(row), delta, static_cast<int>(row.labels.size()));
  if (index < 0) {
    return false;
  }
  PlaySound(ctx, base, delta < 0 ? kSelectorLeftSound : kSelectorRightSound);
  if (row.restart) {
    if (index == row.get()) {
      row.pending.reset();
    } else {
      row.pending = index;
    }
  } else {
    row.set(index);
    if (g_save_settings) {
      g_save_settings();
    }
  }
  RefreshScreen(ctx, base, state, row.rebuild_on_change ? kRefreshScreen : kRefreshValues);
  return true;
}

void CopyGuestString(uint8_t* base, uint32_t address, std::string_view text) {
  std::memcpy(base + address, text.data(), text.size());
  base[address + text.size()] = 0;
}

// Whether the popup manager still shows the popup with this id.
bool PopupShown(uint8_t* base, int32_t id) {
  if (id == kNoPopup) {
    return false;
  }
  const uint32_t manager = REX_LOAD_U32(kPopupManagerPtr);
  if (manager < 0x10000) {
    return false;
  }
  const uint32_t begin = REX_LOAD_U32(manager + kPopupQueueBeginOffset);
  const uint32_t end = REX_LOAD_U32(manager + kPopupQueueEndOffset);
  if (begin < 0x10000 || end < begin || end - begin > kPopupQueueMax * 4) {
    return false;
  }
  for (uint32_t slot = begin; slot < end; slot += 4) {
    const uint32_t popup = REX_LOAD_U32(slot);
    if (popup >= 0x10000 && static_cast<int32_t>(REX_LOAD_U32(popup + kPopupIdOffset)) == id) {
      return true;
    }
  }
  return false;
}

// Shows a two-button popup answered in the sub_8260EA48 hook below, unless
// the popup `id` names is still up. Returns false (and leaves `id` alone)
// when there is no popup manager.
bool ShowPrompt(PPCContext& ctx, uint8_t* base, const Prompt& prompt, int32_t& id) {
  if (PopupShown(base, id)) {
    return true;
  }
  const uint32_t manager = REX_LOAD_U32(kPopupManagerPtr);
  if (manager < 0x10000) {
    return false;
  }
  const uint32_t desc = g_popup_desc;
  ctx.r3.u64 = desc;
  sub_82D0B0B0(ctx, base);
  CopyGuestString(base, desc + kPopupTitleOffset, prompt.title);
  CopyGuestString(base, desc + kPopupMessageOffset, prompt.message);
  REX_STORE_U32(desc + kPopupButtonCountOffset, 2);
  CopyGuestString(base, desc + kPopupButtonOffsets[0], prompt.buttons[0]);
  CopyGuestString(base, desc + kPopupButtonOffsets[1], prompt.buttons[1]);
  REX_STORE_U32(desc + kPopupCallbackOffset, kPopupCallback);
  ctx.r3.u64 = manager;
  ctx.r4.u64 = desc;
  sub_82D0A8D0(ctx, base);
  id = ctx.r3.s32;  // the new popup's id
  REXLOG_INFO("Options menu: {} prompt shown (popup {})", prompt.name, id);
  return true;
}

// "Restart Required: Restart Now / Discard Changes". Returns true when the
// prompt is up (already shown or shown now).
bool ShowRestartPrompt(PPCContext& ctx, uint8_t* base) {
  return ShowPrompt(ctx, base, kRestartPrompt, g_restart_popup_id);
}

// "Quit Skate 3?: Quit / Cancel", from the root Quit Game row.
void ShowQuitPrompt(PPCContext& ctx, uint8_t* base) {
  if (!ShowPrompt(ctx, base, kQuitPrompt, g_quit_popup_id)) {
    REXLOG_WARN("Options menu: no popup manager; Quit Game prompt not shown");
  }
}

// ---- Saves in flight -----------------------------------------------------------

// Every guest content open and close goes through the XDK's XContentCreate /
// XContentClose wrappers (sub_82EDE718 -> XamContentCreateEx, sub_82EDE7A8 ->
// XamContentClose; r4 / r3 = root name). A saved game opened with any
// disposition but "open existing" is being written until it is closed: the
// career autosave writes its files between the two and the close commits
// them. Hooked below; any guest thread.
constexpr uint32_t kContentTypeSavedGame = 1;   // XCONTENT_DATA +4
constexpr uint32_t kDispositionMask = 0xF;      // XContentCreate flags
constexpr uint32_t kDispositionOpenExisting = 3;
constexpr uint32_t kErrorIoPending = 997;       // overlapped open still running
constexpr size_t kRootNameMax = 64;
std::mutex g_saves_mutex;
std::unordered_map<std::string, int> g_open_saves;  // root name -> write opens
std::atomic<int> g_saves_in_flight{0};

std::string GuestRootName(uint8_t* base, uint32_t address) {
  if (address < 0x10000) {
    return {};
  }
  const char* text = reinterpret_cast<const char*>(base + address);
  return std::string(text, strnlen(text, kRootNameMax));
}

void SaveOpened(const std::string& root) {
  std::lock_guard lock(g_saves_mutex);
  ++g_open_saves[root];
  g_saves_in_flight.fetch_add(1, std::memory_order_acq_rel);
  REXLOG_INFO("Saved game '{}' opened for writing", root);
}

void SaveClosed(const std::string& root) {
  std::lock_guard lock(g_saves_mutex);
  const auto it = g_open_saves.find(root);
  if (it == g_open_saves.end()) {
    return;  // a read-only open, or content that is not a saved game
  }
  if (--it->second == 0) {
    g_open_saves.erase(it);
  }
  g_saves_in_flight.fetch_sub(1, std::memory_order_acq_rel);
  REXLOG_INFO("Saved game '{}' closed", root);
}

bool SaveInFlight() { return g_saves_in_flight.load(std::memory_order_acquire) > 0; }

// ---- Quit ------------------------------------------------------------------------

// A save that is still open after this long is treated as stuck (or one this
// tracking never saw close) and no longer holds the quit back.
constexpr auto kQuitSaveWait = std::chrono::seconds(15);
constexpr auto kQuitSavePoll = std::chrono::milliseconds(50);
std::atomic<bool> g_quitting{false};

// Persist the settings, then the overlay's Close Game exit. Both marshal to
// the UI thread in order, so the file is written first. Any thread.
void QuitNow() {
  if (g_save_settings) {
    g_save_settings();
  }
  if (g_quit) {
    g_quit();
  }
}

// Quit confirmed. A save in flight is let finish first (polled off the guest
// thread, so the game keeps running and the save completes). Restart-required
// choices still staged (a restart prompt dismissed without an answer) are not
// applied, as with Discard Changes.
void QuitGame() {
  if (g_quitting.exchange(true)) {
    return;
  }
  if (!SaveInFlight()) {
    REXLOG_INFO("Options menu: Quit Game confirmed; saving settings and closing the game");
    QuitNow();
    return;
  }
  REXLOG_INFO("Options menu: Quit Game confirmed; waiting for the save in progress");
  std::thread([]() {
    const auto deadline = std::chrono::steady_clock::now() + kQuitSaveWait;
    while (SaveInFlight() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(kQuitSavePoll);
    }
    if (SaveInFlight()) {
      REXLOG_WARN("Options menu: save still open after {} s; closing the game anyway",
                  std::chrono::duration_cast<std::chrono::seconds>(kQuitSaveWait).count());
    } else {
      REXLOG_INFO("Options menu: save finished; saving settings and closing the game");
    }
    QuitNow();
  }).detach();
}

void ApplyPendingAndRestart() {
  for (Page& page : g_pages) {
    for (Row& row : page.rows) {
      if (row.pending) {
        row.set(*row.pending);
        row.pending.reset();
      }
    }
  }
  REXLOG_INFO("Options menu: restart-required settings applied; restarting");
  // Both marshal to the UI thread in order: the settings file is written
  // before the restart relaunches the game.
  if (g_save_settings) {
    g_save_settings();
  }
  if (g_restart) {
    g_restart();
  }
}

void DiscardPending() {
  for (Page& page : g_pages) {
    for (Row& row : page.rows) {
      row.pending.reset();
    }
  }
}

// Native hooks run with no incoming register arguments, but the state lookup
// calls into the guest; keep the argument registers as the caller left them
// for the game's own implementation.
struct ArgRegisters {
  explicit ArgRegisters(PPCContext& ctx)
      : ctx_(ctx), r3_(ctx.r3.u64), r4_(ctx.r4.u64), r5_(ctx.r5.u64), r6_(ctx.r6.u64) {}
  void Restore() {
    ctx_.r3.u64 = r3_;
    ctx_.r4.u64 = r4_;
    ctx_.r5.u64 = r5_;
    ctx_.r6.u64 = r6_;
  }
  PPCContext& ctx_;
  uint64_t r3_, r4_, r5_, r6_;
};

}  // namespace
#endif  // SKATE3_HAS_TITLE_UPDATE

void SetOpenSettingsCallback(std::function<void()> callback) {
  g_open_settings = std::move(callback);
}

void SetSaveSettingsCallback(std::function<void()> callback) {
  g_save_settings = std::move(callback);
}

void SetRestartCallback(std::function<void()> callback) { g_restart = std::move(callback); }

void SetQuitCallback(std::function<void()> callback) { g_quit = std::move(callback); }

void Install() {
#if SKATE3_HAS_TITLE_UPDATE
  if (IsInstalled() || !REXCVAR_GET(skate3_options_menu_entry)) {
    return;
  }
  auto* memory = rex::system::kernel_memory();
  if (!memory) {
    return;
  }
  uint8_t* base = memory->virtual_membase();
  if (!ImageMatches(base)) {
    REXLOG_WARN("Options menu: unexpected Options tables in the loaded image; "
                "PC settings rows not added");
    return;
  }
  BuildMenu();
  if (!PublishStrings(memory, base)) {
    g_pages.clear();
    g_root_rows.clear();
    REXLOG_WARN("Options menu: guest string allocation failed; PC settings rows not added");
    return;
  }
  g_type_selector = REX_LOAD_U32(OptionTypeSlot(kSelectorOptionId));
  g_type_option = REX_LOAD_U32(OptionTypeSlot(kOptionTypeId));

  for (size_t i = 0; i < g_root_rows.size(); ++i) {
    const RootRow& root = g_root_rows[i];
    REX_STORE_U32(OptionTitleSlot(root.option_id), root.title_id);
    REX_STORE_U32(OptionTypeSlot(root.option_id), g_type_option);
    for (uint32_t screen : kRootScreens) {
      REX_STORE_U32(LayoutSlot(screen, static_cast<uint32_t>(std::size(kRootLayout) + i)),
                    static_cast<uint32_t>(root.option_id));
    }
  }
  g_installed.store(true, std::memory_order_release);
  size_t rows = 0;
  for (const Page& page : g_pages) {
    rows += page.rows.size();
  }
  REXLOG_INFO("Options menu: {} PC settings rows ({} pages, {} settings) added to the root "
              "Options screen",
              g_root_rows.size(), g_pages.size(), rows);
#endif
}

}  // namespace skate3::options_menu

#if SKATE3_HAS_TITLE_UPDATE
using namespace skate3::options_menu;

// Localizer string lookup by hash (r3 = hash, r4 = out table index), behind
// GetLocaleText and every other localized-text path. Returns a char*.
extern "C" REX_FUNC(sub_8290C1C0) {
  if (IsInstalled()) {
    if (auto it = g_locale.find(ctx.r3.u32); it != g_locale.end()) {
      ctx.r3.u64 = it->second;
      return;
    }
  }
  __imp__sub_8290C1C0(ctx, base);
}

// Popup result callback (r3 = popup id, r4 = button index or -2 when the
// manager flushes its popups). Owned by the Options screen's Video
// Calibration popup; answers only for our prompts' own ids, so any other
// popup sharing this callback always reaches the game's handler.
extern "C" REX_FUNC(sub_8260EA48) {
  if (IsInstalled() && g_quit_popup_id != kNoPopup && ctx.r3.s32 == g_quit_popup_id) {
    g_quit_popup_id = kNoPopup;
    const int32_t button = ctx.r4.s32;
    REXLOG_INFO("Options menu: quit prompt answered (button {})", button);
    if (button == kQuitButton) {
      QuitGame();
    }
    // Cancel or a flush: the popup is gone and the Options screen it covered
    // takes input again.
    return;
  }
  if (IsInstalled() && g_restart_popup_id != kNoPopup && ctx.r3.s32 == g_restart_popup_id) {
    g_restart_popup_id = kNoPopup;
    const int32_t button = ctx.r4.s32;
    REXLOG_INFO("Options menu: restart prompt answered (button {})", button);
    if (button == 0) {
      ApplyPendingAndRestart();
      return;
    }
    if (button == 1) {
      // Discard Changes: drop the staged choices and finish the Back the
      // prompt interrupted (the root Options screen).
      DiscardPending();
      const uint32_t state = OptionsState(ctx, base);
      if (PageOf(base, state)) {
        PlaySound(ctx, base, kBackSound);
        ctx.r3.u64 = state;
        sub_8260D078(ctx, base);
      }
    }
    // Dismissed without a choice (the manager flushing its popups, possibly
    // while the screen goes away): leave the staged choices and the screen
    // alone; the next Back on the page prompts again.
    return;
  }
  __imp__sub_8260EA48(ctx, base);
}

// ---- FrontEndState_Options methods (r3 = state) -------------------------------

// OnMenuSelect(menu, row = r5, item name). Our root rows open their page or
// the overlay, or ask to quit; on our pages A does nothing, as on the game's
// own selectors.
extern "C" REX_FUNC(sub_8260D290) {
  if (IsInstalled()) {
    const uint32_t state = ctx.r3.u32;
    if (PageOf(base, state)) {
      return;
    }
    if (const RootRow* root = RootRowAt(base, state, ctx.r5.u32)) {
      PlaySound(ctx, base, kSubmenuSound);
      if (root->page >= 0) {
        REX_STORE_U32(state + kStateScreenOffset,
                      kFirstPageScreen + static_cast<uint32_t>(root->page));
        RefreshScreen(ctx, base, state, kRefreshScreen);
      } else if (root->page == kQuitPage) {
        ShowQuitPrompt(ctx, base);
      } else if (g_open_settings) {
        g_open_settings();
      }
      return;
    }
  }
  __imp__sub_8260D290(ctx, base);
}

// Input handler (r5 = event). On our pages left/right step selectors, and
// Back either prompts to restart (staged restart-required changes) or keeps
// the game's handling (the root Options screen). Nothing else is consumed, so
// the game never indexes its tables with a page's screen id. On the game's
// screens left/right on one of our root rows do nothing (the game would step
// the selector a borrowed id still is there).
extern "C" REX_FUNC(sub_8260C258) {
  if (IsInstalled()) {
    const uint32_t state = ctx.r3.u32;
    const uint32_t event = ctx.r5.u32;
    if ((event == kInputLeft || event == kInputRight) && !PageOf(base, state) &&
        RootRowAt(base, state, REX_LOAD_U32(state + kStateRowOffset))) {
      ctx.r3.u64 = 0;
      return;
    }
    if (Page* page = PageOf(base, state)) {
      if (event == kInputBack) {
        if (HasPendingRestart() && ShowRestartPrompt(ctx, base)) {
          ctx.r3.u64 = 1;
          return;
        }
        __imp__sub_8260C258(ctx, base);
        return;
      }
      Row* row = RowAt(*page, REX_LOAD_U32(state + kStateRowOffset));
      const bool stepped = (event == kInputLeft || event == kInputRight) && row &&
                           RowEnabled(*row) &&
                           StepRow(ctx, base, state, *row, event == kInputLeft ? -1 : 1);
      ctx.r3.u64 = stepped ? 1 : 0;
      return;
    }
  }
  __imp__sub_8260C258(ctx, base);
}

// Row title string ID (r4 = row).
extern "C" REX_FUNC(sub_8260D718) {
  if (IsInstalled()) {
    if (Page* page = PageOf(base, ctx.r3.u32)) {
      const Row* row = RowAt(*page, ctx.r4.u32);
      ctx.r3.u64 = row ? row->title_id : kStringEmpty;
      return;
    }
  }
  __imp__sub_8260D718(ctx, base);
}

// Row integer value (r4 = row): the shown label index.
extern "C" REX_FUNC(sub_8260D7E8) {
  if (IsInstalled()) {
    if (Page* page = PageOf(base, ctx.r3.u32)) {
      const Row* row = RowAt(*page, ctx.r4.u32);
      ctx.r3.s64 = row ? ShownLabel(*row).value_or(0) : 0;
      return;
    }
  }
  __imp__sub_8260D7E8(ctx, base);
}

// Row value string ID (r4 = row). Our root rows have no value.
extern "C" REX_FUNC(sub_8260D968) {
  if (IsInstalled()) {
    if (RootRowAt(base, ctx.r3.u32, ctx.r4.u32)) {
      ctx.r3.u64 = kStringEmpty;
      return;
    }
    if (Page* page = PageOf(base, ctx.r3.u32)) {
      const Row* row = RowAt(*page, ctx.r4.u32);
      const std::optional<int> label = row ? ShownLabel(*row) : std::nullopt;
      ctx.r3.u64 = label && static_cast<size_t>(*label) < row->label_ids.size()
                       ? row->label_ids[*label]
                       : kStringEmpty;
      return;
    }
  }
  __imp__sub_8260D968(ctx, base);
}

// Row enabled (r5 = row).
extern "C" REX_FUNC(sub_8260D648) {
  if (IsInstalled()) {
    if (Page* page = PageOf(base, ctx.r3.u32)) {
      const Row* row = RowAt(*page, ctx.r5.u32);
      ctx.r3.u64 = row && RowEnabled(*row) ? 1 : 0;
      return;
    }
  }
  __imp__sub_8260D648(ctx, base);
}

// ---- GameSettings_* APT natives that read the tables directly ------------------

// GameSettings_GetNumOptions().
extern "C" REX_FUNC(sub_825A6F70) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    if (Page* page = PageOf(base, OptionsState(ctx, base))) {
      AptReturnInt(ctx, base, static_cast<int32_t>(page->rows.size()));
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6F70(ctx, base);
}

// GameSettings_GetOptionType(row): every page row is a selector.
extern "C" REX_FUNC(sub_825A6CE0) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    if (PageOf(base, OptionsState(ctx, base))) {
      AptReturnString(ctx, base, g_type_selector);
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6CE0(ctx, base);
}

// GameSettings_GetScreenTitle().
extern "C" REX_FUNC(sub_825A6DA8) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    if (Page* page = PageOf(base, OptionsState(ctx, base))) {
      AptReturnString(ctx, base, page->title_id);
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6DA8(ctx, base);
}

// GameSettings_GetValueDescription(row).
extern "C" REX_FUNC(sub_825A6E88) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    if (Page* page = PageOf(base, OptionsState(ctx, base))) {
      const Row* row = RowAt(*page, static_cast<uint32_t>(AptArgInt(ctx, base, 0)));
      AptReturnString(ctx, base, row ? row->desc_id : kStringEmpty);
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6E88(ctx, base);
}

// ---- XContentCreate / XContentClose (saves in flight) -------------------------------

// XContentCreate(user r3, root name r4, XCONTENT_DATA* r5, flags r6, ...).
extern "C" REX_FUNC(sub_82EDE718) {
  const uint32_t content = ctx.r5.u32;
  const bool write = content >= 0x10000 &&
                     REX_LOAD_U32(content + 4) == kContentTypeSavedGame &&
                     (ctx.r6.u32 & kDispositionMask) != kDispositionOpenExisting;
  const std::string root = write ? GuestRootName(base, ctx.r4.u32) : std::string();
  __imp__sub_82EDE718(ctx, base);
  if (write && (ctx.r3.u32 == 0 || ctx.r3.u32 == kErrorIoPending)) {
    SaveOpened(root);
  }
}

// XContentClose(root name r3, overlapped r4).
extern "C" REX_FUNC(sub_82EDE7A8) {
  const std::string root = GuestRootName(base, ctx.r3.u32);
  __imp__sub_82EDE7A8(ctx, base);
  SaveClosed(root);
}

// GameSettings_GetButtonHelpTextA(): no A prompt on page rows (all
// selectors), as the game does for its own selectors.
extern "C" REX_FUNC(sub_825A6FE0) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    if (PageOf(base, OptionsState(ctx, base))) {
      AptReturnString(ctx, base, kStringEmpty);
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6FE0(ctx, base);
}
#endif

#include "harness/harness.h"

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>

#include "skate3_native_scene.h"
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/ppc/context.h>
#include <rex/ui/renderdoc_capture.h>
#include <spdlog/sinks/base_sink.h>

#include "harness/harness_crash.h"
#include "harness/harness_game.h"
#include "harness/harness_pad.h"
#include "harness/harness_png.h"
#include "harness/harness_script.h"
#include "native/skate3_native_guest_read.h"
#include "skate3_version.h"

static_assert(SKATE3_HAS_TITLE_UPDATE,
              "the harness hooks (XInputGetState wrapper, FE manager) are TU3 addresses");

REXCVAR_DEFINE_BOOL(skate3_harness, false, "Skate 3",
                    "Scenario harness live mode: the <skate3_harness_out>/inbox command channel "
                    "and input recording (Ctrl+Shift+R), without a scenario")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(skate3_harness_scenario, "", "Skate 3",
                      "Scenario harness: run this scenario TOML unattended and exit with its "
                      "result code (README: Scenario harness)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(skate3_harness_out, "harness", "Skate 3",
                      "Scenario harness output folder (run folders, inbox/, outbox/), relative "
                      "to the working directory")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace skate3::native_scene {
// skate3_native_scene.cpp: the main perspective ViewCamera of the last
// built frame (0 when the native renderer has not built one).
uint32_t MainViewCamera();
}  // namespace skate3::native_scene

REXCVAR_DECLARE(bool, skate3_native_render_scene);

namespace skate3::harness {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// FrontEndManager singleton pointer and its push-state stack (eastl vector of
// 20-byte records, screen id at +0); see skate3_demo_path.cpp.
constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
constexpr uint32_t kFeStackBegin = 0x210;
constexpr uint32_t kFeStackEnd = 0x214;
constexpr uint32_t kFeRecordSize = 20;
// PresentationEntity::m_MatLtoW (Matrix44, row-vector: translation in row 3).
constexpr uint32_t kEntityMatLtoW = 352;
// How long a skater entity stays a candidate after its last bind.
constexpr uint64_t kSkaterKeepFrames = 120;

std::atomic<bool> g_enabled{false};

// ------------------------------------------------------------ guest reads

bool ReadU32(uint8_t* base, uint32_t addr, uint32_t* out) {
  uint32_t raw = 0;
  if (addr == 0 || !native_scene::GuestTryCopy(&raw, base + addr, sizeof(raw))) {
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

bool ReadF32x16(uint8_t* base, uint32_t addr, float out[16]) {
  uint32_t raw[16];
  if (addr == 0 || !native_scene::GuestTryCopy(raw, base + addr, sizeof(raw))) {
    return false;
  }
  for (int i = 0; i < 16; ++i) {
    const uint32_t v = __builtin_bswap32(raw[i]);
    std::memcpy(&out[i], &v, 4);
  }
  return true;
}

// ------------------------------------------------------------ run state

struct GameState {
  uint64_t frame = 0;
  double t = 0.0;  // seconds since the harness started
  double fps = 0.0;
  uint32_t presence = 0;
  int64_t fe_top = -1;
  uint32_t fe_depth = 0;
  uint32_t skater = 0;
  uint32_t skater_candidates = 0;  // skater entities bound recently
  bool skater_valid = false;
  float skater_pos[3] = {};
  double skater_speed = 0.0;    // units/s, 3D
  double skater_hspeed = 0.0;   // units/s, horizontal (x/z)
  bool cam_valid = false;
  float cam_pos[3] = {};
  bool paused = false;
  bool replay_active = false;  // PhotoReplayController::Update ticking
  bool movie_active = false;   // MovieDecoder::Decode ticking
  // Full FE push-state stack: screen id and the record's second field
  // (-1 except in the photo editor).
  std::vector<std::pair<int32_t, int32_t>> fe_stack;
};

// Frames a heartbeat stays "active" after its last tick.
constexpr uint64_t kHeartbeatFrames = 10;
std::atomic<uint64_t> g_replay_tick_frame{0};
std::atomic<uint64_t> g_movie_tick_frame{0};

std::string FeStackJson(const std::vector<std::pair<int32_t, int32_t>>& stack) {
  std::string out = "[";
  for (size_t i = 0; i < stack.size(); ++i) {
    if (i) {
      out += ",";
    }
    // An entry with a second field is a string ("22:0") to keep the line JSON.
    if (stack[i].second != -1) {
      out += "\"" + std::to_string(stack[i].first) + ":" + std::to_string(stack[i].second) + "\"";
    } else {
      out += std::to_string(stack[i].first);
    }
  }
  return out + "]";
}

std::string StateJson(const GameState& s, std::string_view tag = {}) {
  char buf[640];
  int n = std::snprintf(
      buf, sizeof(buf),
      "{\"frame\":%llu,\"t\":%.3f,\"fps\":%.1f,\"presence\":%u,\"fe_top\":%lld,"
      "\"fe_depth\":%u,\"paused\":%s,\"skaters\":%u,\"replay\":%s,\"movie\":%s",
      static_cast<unsigned long long>(s.frame), s.t, s.fps, s.presence,
      static_cast<long long>(s.fe_top), s.fe_depth, s.paused ? "true" : "false",
      s.skater_candidates, s.replay_active ? "true" : "false",
      s.movie_active ? "true" : "false");
  std::string out(buf, n > 0 ? static_cast<size_t>(n) : 0);
  out += ",\"fe\":" + FeStackJson(s.fe_stack);
  if (s.skater_valid) {
    n = std::snprintf(buf, sizeof(buf),
                      ",\"skater\":{\"entity\":\"0x%08X\",\"pos\":[%.3f,%.3f,%.3f],"
                      "\"speed\":%.3f,\"hspeed\":%.3f}",
                      s.skater, s.skater_pos[0], s.skater_pos[1], s.skater_pos[2],
                      s.skater_speed, s.skater_hspeed);
    out.append(buf, n > 0 ? static_cast<size_t>(n) : 0);
  }
  if (s.cam_valid) {
    n = std::snprintf(buf, sizeof(buf), ",\"cam\":[%.3f,%.3f,%.3f]", s.cam_pos[0], s.cam_pos[1],
                      s.cam_pos[2]);
    out.append(buf, n > 0 ? static_cast<size_t>(n) : 0);
  }
  if (!tag.empty()) {
    out += ",\"tag\":\"" + JsonEscape(tag) + "\"";
  }
  out += "}";
  return out;
}

struct Run {
  // Configuration.
  std::optional<Scenario> scenario;
  fs::path out_root;
  fs::path dir;
  Clock::time_point t0 = Clock::now();
  CaptureFn capture;
  CloseFn close;

  // Frame/poll clock and game state (guarded by mu).
  std::mutex mu;
  std::condition_variable cv;  // frame advanced, poll advanced, pause changes
  uint64_t frame = 0;
  std::atomic<uint64_t> frame_atomic{0};  // frame, readable without mu
  uint64_t poll = 0;
  GameState state;
  int64_t state_every = 10;
  Clock::time_point last_frame_time = Clock::now();
  uint32_t last_skater = 0;
  float last_pos[3] = {};
  bool have_last_pos = false;
  Clock::time_point last_pos_time;

  // Skater entities and the frame each was last bound on. Binding runs on
  // the render side and does not land between every pair of frame
  // boundaries, so entities stay candidates for kSkaterKeepFrames.
  std::mutex skater_mu;
  std::vector<std::pair<uint32_t, uint64_t>> skaters_seen;

  // Scripted pad (input step).
  bool script_active = false;
  PadState script_pad;
  bool script_override = true;
  uint64_t script_until_frame = 0;

  // Playback.
  std::shared_ptr<const Recording> play;
  bool play_override = true;
  uint64_t play_start_poll = 0;
  uint64_t play_end_poll = 0;
  size_t play_cursor = 0;
  bool play_active = false;

  // Recording.
  bool recording = false;
  std::ofstream rec_out;
  fs::path rec_path;
  uint64_t rec_start_poll = 0;
  uint64_t rec_start_frame = 0;
  PadState rec_last;
  bool rec_any = false;
  uint64_t rec_rows = 0;

  // Packet number bias for harness-modified pad states.
  uint32_t packet_bias = 0;
  PadState last_out;

  // Pause / frame stepping.
  bool paused = false;
  int64_t step_budget = 0;

  // Files.
  std::mutex file_mu;
  std::ofstream events;
  std::ofstream state_out;
  std::ofstream log_out;

  // Counters.
  std::atomic<uint64_t> warnings{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> shots{0};
  std::atomic<uint64_t> validation{0};  // Vulkan validation-layer messages
  std::ofstream validation_out;         // validation.txt, opened on the first
  std::vector<std::string> captures;    // RenderDoc .rdc paths (guarded by mu)
  std::vector<std::string> failures;  // guarded by mu

  // Threads.
  std::atomic<bool> stop{false};
  std::atomic<bool> finished{false};
  std::thread runner;
  std::thread inbox;
  std::thread watchdog;
  std::thread encoder;

  // PNG encode queue.
  struct Shot {
    fs::path path;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
  };
  std::mutex shot_mu;
  std::condition_variable shot_cv;
  std::deque<Shot> shot_queue;
};

Run* g_run = nullptr;

double Seconds(const Run& r) {
  return std::chrono::duration<double>(Clock::now() - r.t0).count();
}

void Event(Run& r, std::string_view kind, const std::string& fields_json) {
  uint64_t frame;
  {
    std::lock_guard lk(r.mu);
    frame = r.frame;
  }
  char head[160];
  std::snprintf(head, sizeof(head), "{\"t\":%.3f,\"frame\":%llu,\"kind\":\"%s\"", Seconds(r),
                static_cast<unsigned long long>(frame), std::string(kind).c_str());
  std::lock_guard lk(r.file_mu);
  if (r.events) {
    r.events << head;
    if (!fields_json.empty()) {
      r.events << "," << fields_json;
    }
    r.events << "}\n";
    r.events.flush();
  }
}

// ------------------------------------------------------------ log tap

struct LogWaiter {
  std::regex re;
  bool hit = false;
};

class LogTap final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  explicit LogTap(Run* run) : run_(run) {}

  std::shared_ptr<LogWaiter> AddWaiter(const std::string& pattern) {
    auto w = std::make_shared<LogWaiter>();
    w->re = std::regex(pattern);
    std::lock_guard lk(waiters_mu_);
    waiters_.push_back(w);
    return w;
  }
  void RemoveWaiter(const std::shared_ptr<LogWaiter>& w) {
    std::lock_guard lk(waiters_mu_);
    waiters_.erase(std::remove(waiters_.begin(), waiters_.end(), w), waiters_.end());
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    if (msg.level >= spdlog::level::err) {
      run_->errors.fetch_add(1, std::memory_order_relaxed);
    } else if (msg.level == spdlog::level::warn) {
      run_->warnings.fetch_add(1, std::memory_order_relaxed);
    }
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    const std::string line(formatted.data(), formatted.size());
    // Validation-layer messages, as the SDK formats them:
    // "Vulkan <severity> (Validation, ID ..." from the debug messenger, and
    // "D3D12 <severity> (debug layer, ID ..." from the D3D12 info queue.
    const std::string_view payload(msg.payload.data(), msg.payload.size());
    const auto within = [&](std::string_view prefix, std::string_view tag) {
      const size_t at = payload.find(tag);
      return payload.substr(0, prefix.size()) == prefix && at != std::string_view::npos &&
             at < 40;
    };
    const bool validation =
        within("Vulkan ", "Validation") || within("D3D12 ", "(debug layer");
    {
      std::lock_guard lk(run_->file_mu);
      if (run_->log_out) {
        run_->log_out << line;
      }
      if (validation) {
        run_->validation.fetch_add(1, std::memory_order_relaxed);
        if (!run_->validation_out.is_open()) {
          run_->validation_out.open(run_->dir / "validation.txt",
                                    std::ios::binary | std::ios::app);
        }
        run_->validation_out << line;
      }
    }
    std::lock_guard lk(waiters_mu_);
    if (!waiters_.empty()) {
      const std::string payload(msg.payload.data(), msg.payload.size());
      for (auto& w : waiters_) {
        if (!w->hit && std::regex_search(payload, w->re)) {
          w->hit = true;
        }
      }
    }
  }
  void flush_() override {
    std::lock_guard lk(run_->file_mu);
    if (run_->log_out) {
      run_->log_out.flush();
    }
  }

 private:
  Run* run_;
  std::mutex waiters_mu_;
  std::vector<std::shared_ptr<LogWaiter>> waiters_;
};

std::shared_ptr<LogTap> g_tap;

// ------------------------------------------------------------ finish

std::string ResultJson(Run& r, int code, std::string_view status, std::string_view reason) {
  std::ostringstream os;
  os << "{\n  \"status\": \"" << JsonEscape(status) << "\",\n  \"code\": " << code
     << ",\n  \"reason\": \"" << JsonEscape(reason) << "\",\n  \"scenario\": \""
     << JsonEscape(r.scenario ? r.scenario->name : std::string("live")) << "\",\n";
  // Which build produced the run (A/B comparisons, run_scenario.py symbolize).
  os << "  \"version\": \"" << SKATE3_VERSION_STRING << "\",\n  \"platform\": \""
     << SKATE3_BUILD_PLATFORM << "\",\n  \"build_config\": \"" << SKATE3_BUILD_CONFIG
     << "\",\n";
  {
    std::lock_guard lk(r.mu);
    os << "  \"frames\": " << r.frame << ",\n  \"polls\": " << r.poll << ",\n";
    os << "  \"failures\": [";
    for (size_t i = 0; i < r.failures.size(); ++i) {
      os << (i ? ", " : "") << "\"" << JsonEscape(r.failures[i]) << "\"";
    }
    os << "],\n";
    os << "  \"last_state\": " << StateJson(r.state) << ",\n";
    os << "  \"captures\": [";
    for (size_t i = 0; i < r.captures.size(); ++i) {
      os << (i ? ", " : "") << "\"" << JsonEscape(r.captures[i]) << "\"";
    }
    os << "],\n";
  }
  // GetFlagByName: "" when the cvar does not exist (backend not built in).
  // Each is only meaningful for the backend in use, which the run reports.
  const bool vk_validation = rex::cvar::GetFlagByName("vulkan_validation_enabled") == "true";
  const bool d3d12_debug = rex::cvar::GetFlagByName("d3d12_debug") == "true";
  os << "  \"validation_enabled\": " << (vk_validation || d3d12_debug ? "true" : "false")
     << ",\n  \"validation_layers\": \""
     << (vk_validation && d3d12_debug ? "vulkan+d3d12"
         : vk_validation              ? "vulkan"
         : d3d12_debug                ? "d3d12"
                                      : "")
     << "\",\n  \"gpu_backend\": \"" << JsonEscape(rex::cvar::GetFlagByName("gpu_backend"))
     << "\",\n  \"validation_messages\": " << r.validation.load() << ",\n";
  os << "  \"seconds\": " << Seconds(r) << ",\n  \"warnings\": " << r.warnings.load()
     << ",\n  \"errors\": " << r.errors.load() << ",\n  \"screenshots\": " << r.shots.load()
     << ",\n  \"dir\": \"" << JsonEscape(r.dir.generic_string()) << "\"\n}\n";
  return os.str();
}

void DrainShots(Run& r, std::chrono::milliseconds limit) {
  const auto deadline = Clock::now() + limit;
  std::unique_lock lk(r.shot_mu);
  r.shot_cv.notify_all();
  r.shot_cv.wait_until(lk, deadline, [&] { return r.shot_queue.empty(); });
}

[[noreturn]] void FinishAndExit(Run& r, int code, std::string_view status,
                                std::string_view reason) {
  bool expected = false;
  if (!r.finished.compare_exchange_strong(expected, true)) {
    // Another thread is already exiting.
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  REXLOG_INFO("harness: finished status={} code={} reason={}", status, code, reason);
  {
    // A paused game would hold the guest thread forever; let it go.
    std::lock_guard lk(r.mu);
    r.paused = false;
  }
  r.cv.notify_all();
  DrainShots(r, std::chrono::seconds(20));
  {
    std::lock_guard lk(r.mu);
    if (r.recording) {
      r.rec_out.flush();
    }
  }
  const std::string result = ResultJson(r, code, status, reason);
  {
    std::ofstream out(r.dir / "result.json", std::ios::binary | std::ios::trunc);
    out << result;
  }
  Event(r, "finish",
        "\"status\":\"" + JsonEscape(status) + "\",\"code\":" + std::to_string(code) +
            ",\"reason\":\"" + JsonEscape(reason) + "\"");
  rex::FlushLogging();
  {
    std::lock_guard lk(r.file_mu);
    r.events.flush();
    r.state_out.flush();
    r.log_out.flush();
  }
  std::fflush(nullptr);
  if (r.close && rex::ui::renderdoc::Attached()) {
    // Under RenderDoc, a hard exit right after a capture left skate3.exe stuck
    // in the GPU driver, unkillable until a reboot (Windows, RTX 4070, RenderDoc
    // 1.46). Shut down normally, so the device and RenderDoc are torn down in
    // order. The process exit code is then the app's; result.json has the real
    // one. A stuck shutdown still ends with the hard exit.
    REXLOG_INFO("harness: RenderDoc attached, closing the game normally");
    rex::FlushLogging();
    std::thread([code] {
      std::this_thread::sleep_for(std::chrono::seconds(30));
      std::_Exit(code);
    }).detach();
    r.close();
    // This thread stays parked; Shutdown() detaches it instead of joining.
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  std::_Exit(code);
}

// ------------------------------------------------------------ screenshots

void EncoderLoop(Run& r) {
  for (;;) {
    Run::Shot shot;
    {
      std::unique_lock lk(r.shot_mu);
      r.shot_cv.wait(lk, [&] { return !r.shot_queue.empty() || r.stop.load(); });
      if (r.shot_queue.empty()) {
        return;
      }
      shot = std::move(r.shot_queue.front());
    }
    std::string error;
    if (WritePng(shot.path, shot.rgba.data(), shot.w, shot.h, &error)) {
      r.shots.fetch_add(1, std::memory_order_relaxed);
    } else {
      REXLOG_WARN("harness: screenshot {} failed: {}", shot.path.string(), error);
    }
    {
      std::lock_guard lk(r.shot_mu);
      r.shot_queue.pop_front();
    }
    r.shot_cv.notify_all();
  }
}

bool WaitFrames(Run& r, int64_t frames, double timeout_s);

// Captures `count` frames `every_frames` apart; returns the file names.
bool TakeScreenshots(Run& r, const std::string& tag, int64_t count, int64_t every_frames,
                     std::vector<std::string>* names, std::string* error) {
  if (!r.capture) {
    *error = "no frame capture available";
    return false;
  }
  fs::create_directories(r.dir / "shots");
  for (int64_t k = 0; k < count; ++k) {
    if (k > 0 && every_frames > 0 && !WaitFrames(r, every_frames, 30.0)) {
      *error = "game stopped presenting frames";
      return false;
    }
    Run::Shot shot;
    if (!r.capture(shot.rgba, shot.w, shot.h) || shot.w == 0 || shot.h == 0) {
      *error = "frame capture failed";
      return false;
    }
    uint64_t frame;
    {
      std::lock_guard lk(r.mu);
      frame = r.frame;
    }
    char name[160];
    std::snprintf(name, sizeof(name), "f%07llu_%s%s%02lld.png",
                  static_cast<unsigned long long>(frame), tag.empty() ? "" : tag.c_str(),
                  tag.empty() ? "" : "_", static_cast<long long>(k));
    shot.path = r.dir / "shots" / name;
    names->push_back(std::string("shots/") + name);
    {
      // Back-pressure: a burst at every frame outruns the PNG encoder.
      std::unique_lock lk(r.shot_mu);
      r.shot_cv.wait(lk, [&] { return r.shot_queue.size() < 24; });
      r.shot_queue.push_back(std::move(shot));
    }
    r.shot_cv.notify_all();
  }
  return true;
}

// ------------------------------------------------------------ waiting

// Waits until pred() holds (checked under r.mu) or the timeout passes.
// The F5 switch: native scene renderer or the emulated GPU. Returns false if
// the switch is refused (the native layer was not enabled at boot).
bool SetRenderer(bool native) {
  if (REXCVAR_GET(skate3_native_render_scene) != native) {
    native_scene::ToggleSceneEnabled();
  }
  return REXCVAR_GET(skate3_native_render_scene) == native;
}

template <typename Pred>
bool WaitUntil(Run& r, double timeout_s, Pred pred) {
  const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                           std::chrono::duration<double>(timeout_s));
  std::unique_lock lk(r.mu);
  while (!pred()) {
    if (r.stop.load()) {
      return false;
    }
    if (r.cv.wait_until(lk, std::min(deadline, Clock::now() + std::chrono::milliseconds(50))) ==
            std::cv_status::timeout &&
        Clock::now() >= deadline) {
      return pred();
    }
  }
  return true;
}

bool WaitFrames(Run& r, int64_t frames, double timeout_s) {
  uint64_t target;
  {
    std::lock_guard lk(r.mu);
    target = r.frame + static_cast<uint64_t>(std::max<int64_t>(0, frames));
  }
  return WaitUntil(r, timeout_s, [&] { return r.frame >= target; });
}

// ------------------------------------------------------------ steps

struct Outcome {
  bool ok = true;
  std::string error;
  std::string result_json;  // extra fields for events / replies
};

Outcome Fail(std::string why) {
  Outcome o;
  o.ok = false;
  o.error = std::move(why);
  return o;
}

std::string ReadFileText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

fs::path ResolveInput(const Run& r, const std::string& file) {
  fs::path p(file);
  if (p.is_absolute() || fs::exists(p)) {
    return p;
  }
  if (r.scenario && !r.scenario->path.empty()) {
    const fs::path beside = fs::path(r.scenario->path).parent_path() / p;
    if (fs::exists(beside)) {
      return beside;
    }
  }
  return r.dir / p;
}

bool StatePredicate(const GameState& s, const Step& st, std::string* why) {
  if (st.presence && static_cast<int64_t>(s.presence) != *st.presence) {
    *why = "presence is " + std::to_string(s.presence);
    return false;
  }
  if (st.fe_screen && s.fe_top != *st.fe_screen) {
    *why = "FE top screen is " + std::to_string(s.fe_top);
    return false;
  }
  if (st.speed_gt && !(s.skater_valid && s.skater_speed > *st.speed_gt)) {
    *why = s.skater_valid ? "skater speed is " + std::to_string(s.skater_speed)
                          : "no skater found";
    return false;
  }
  if (st.speed_lt && !(s.skater_valid && s.skater_speed < *st.speed_lt)) {
    *why = s.skater_valid ? "skater speed is " + std::to_string(s.skater_speed)
                          : "no skater found";
    return false;
  }
  return true;
}

void StartRecording(Run& r, const std::string& file, const std::string& note) {
  std::lock_guard lk(r.mu);
  if (r.recording) {
    return;
  }
  std::string name = file;
  if (name.empty()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "input_f%07llu.s3rec",
                  static_cast<unsigned long long>(r.frame));
    name = buf;
  }
  r.rec_path = r.dir / name;
  r.rec_out.open(r.rec_path, std::ios::binary | std::ios::trunc);
  const std::string header = "{\"scenario\":\"" +
                             JsonEscape(r.scenario ? r.scenario->name : "live") +
                             "\",\"note\":\"" + JsonEscape(note) + "\",\"start\":" +
                             StateJson(r.state) + "}";
  r.rec_out << FormatRecHeader(header);
  r.rec_start_poll = r.poll;
  r.rec_start_frame = r.frame;
  r.rec_any = false;
  r.rec_rows = 0;
  r.recording = true;
}

std::string StopRecording(Run& r) {
  std::lock_guard lk(r.mu);
  if (!r.recording) {
    return {};
  }
  // A closing row marks the recording's length.
  RecRow row{r.poll - r.rec_start_poll, r.frame - r.rec_start_frame, r.rec_last};
  r.rec_out << FormatRecRow(row);
  r.rec_out.close();
  r.recording = false;
  return r.rec_path.filename().string();
}

Outcome Execute(Run& r, const Step& st);
std::string SafeName(std::string s);

Outcome ExecuteWait(Run& r, const Step& st) {
  if (st.ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(*st.ms));
    return {};
  }
  if (st.frames) {
    if (!WaitFrames(r, *st.frames, st.timeout_s)) {
      return Fail("timed out waiting for " + std::to_string(*st.frames) + " frames");
    }
    return {};
  }
  std::shared_ptr<LogWaiter> waiter;
  if (!st.log.empty()) {
    try {
      waiter = g_tap->AddWaiter(st.log);
    } catch (const std::regex_error& e) {
      return Fail(std::string("bad log regex: ") + e.what());
    }
  }
  std::string why;
  const bool ok = WaitUntil(r, st.timeout_s, [&] {
    if (st.gameplay && !(r.state.presence == 1 && r.state.skater_valid)) {
      why = "not in gameplay";
      return false;
    }
    if (waiter && !waiter->hit) {
      why = "no log line matched";
      return false;
    }
    return StatePredicate(r.state, st, &why);
  });
  if (waiter) {
    g_tap->RemoveWaiter(waiter);
  }
  if (!ok) {
    return Fail("wait timed out after " + std::to_string(st.timeout_s) + " s: " + why);
  }
  return {};
}

Outcome ExecuteExpect(Run& r, const Step& st) {
  if (!st.log.empty() || !st.no_log.empty()) {
    rex::FlushLogging();
    const std::string text = ReadFileText(r.dir / "log.txt");
    try {
      if (!st.log.empty() && !std::regex_search(text, std::regex(st.log))) {
        return Fail("expected log line /" + st.log + "/ not found");
      }
      if (!st.no_log.empty()) {
        std::smatch m;
        if (std::regex_search(text, m, std::regex(st.no_log))) {
          return Fail("unexpected log line /" + st.no_log + "/: " + m.str());
        }
      }
    } catch (const std::regex_error& e) {
      return Fail(std::string("bad regex: ") + e.what());
    }
  }
  if (st.presence || st.speed_gt || st.speed_lt) {
    std::lock_guard lk(r.mu);
    std::string why;
    if (!StatePredicate(r.state, st, &why)) {
      return Fail("expectation failed: " + why);
    }
  }
  return {};
}

Outcome Execute(Run& r, const Step& st) {
  switch (st.verb) {
    case Verb::kWait:
      return ExecuteWait(r, st);

    case Verb::kInput: {
      uint64_t until;
      {
        std::lock_guard lk(r.mu);
        r.script_pad = st.pad;
        r.script_override = st.override_pad;
        r.script_until_frame = r.frame + static_cast<uint64_t>(*st.frames);
        r.script_active = true;
        until = r.script_until_frame;
      }
      if (!st.async &&
          !WaitUntil(r, static_cast<double>(*st.frames) / 10.0 + 30.0,
                     [&] { return r.frame >= until; })) {
        return Fail("game stopped presenting frames during input");
      }
      return {};
    }

    case Verb::kPlay: {
      const fs::path path = ResolveInput(r, st.file);
      std::string error;
      auto rec = ParseRecording(ReadFileText(path), &error);
      if (!rec) {
        return Fail(path.string() + ": " + error);
      }
      if (rec->rows.empty()) {
        return Fail(path.string() + ": no rows");
      }
      auto shared = std::make_shared<const Recording>(std::move(*rec));
      uint64_t end;
      {
        std::lock_guard lk(r.mu);
        r.play = shared;
        r.play_override = st.override_pad;
        r.play_start_poll = r.poll;
        r.play_end_poll = r.poll + shared->rows.back().poll;
        r.play_cursor = 0;
        r.play_active = true;
        end = r.play_end_poll;
      }
      Outcome o;
      o.result_json = "\"rows\":" + std::to_string(shared->rows.size()) +
                      ",\"polls\":" + std::to_string(shared->rows.back().poll);
      if (!st.async) {
        const double seconds = static_cast<double>(shared->rows.back().poll) / 30.0 + 60.0;
        if (!WaitUntil(r, seconds, [&] { return r.poll > end || !r.play_active; })) {
          return Fail("playback did not finish (is the game polling input?)");
        }
      }
      return o;
    }

    case Verb::kRecord: {
      Outcome o;
      if (st.start) {
        StartRecording(r, st.file, st.note);
        std::lock_guard lk(r.mu);
        o.result_json = "\"file\":\"" + JsonEscape(r.rec_path.filename().string()) + "\"";
      } else {
        const std::string name = StopRecording(r);
        o.result_json = "\"file\":\"" + JsonEscape(name) + "\"";
      }
      return o;
    }

    case Verb::kCvar: {
      for (const auto& [name, value] : st.set) {
        if (!rex::cvar::SetFlagByName(name, value)) {
          return Fail("cvar " + name + " = " + value + " was rejected (unknown or init-only)");
        }
      }
      return {};
    }

    case Verb::kScreenshot: {
      std::vector<std::string> names;
      std::string error;
      if (!TakeScreenshots(r, st.tag, st.count, st.every_frames, &names, &error)) {
        return Fail(error);
      }
      Outcome o;
      o.result_json = "\"files\":[";
      for (size_t i = 0; i < names.size(); ++i) {
        o.result_json += (i ? ",\"" : "\"") + JsonEscape(names[i]) + "\"";
      }
      o.result_json += "]";
      return o;
    }

    case Verb::kState: {
      std::lock_guard lk(r.mu);
      if (st.every_frames_given) {
        r.state_every = st.every_frames;
      }
      const std::string line = StateJson(r.state, st.tag.empty() ? "state" : st.tag);
      {
        std::lock_guard fl(r.file_mu);
        r.state_out << line << "\n";
        r.state_out.flush();
      }
      Outcome o;
      o.result_json = "\"state\":" + line;
      return o;
    }

    case Verb::kMark:
      REXLOG_INFO("harness: mark: {}", st.text);
      return {};

    case Verb::kSnapshot: {
      rex::cvar::SetFlagByName("skate3_native_render_capture_hotkeys", "true");
      const fs::path dir{rex::cvar::Query<std::string>("skate3_native_render_snapshot_dir")};
      std::error_code ec;
      fs::create_directories(dir, ec);
      std::ofstream(dir / "trigger") << "harness\n";
      Outcome o;
      o.result_json = "\"dir\":\"" + JsonEscape(fs::absolute(dir, ec).generic_string()) + "\"";
      return o;
    }

    case Verb::kPause: {
      {
        std::lock_guard lk(r.mu);
        r.paused = true;
        r.step_budget = 0;
      }
      r.cv.notify_all();
      return {};
    }

    case Verb::kStep: {
      uint64_t target;
      {
        std::lock_guard lk(r.mu);
        if (!r.paused) {
          return Fail("step needs the game paused first");
        }
        r.step_budget += *st.frames;
        target = r.frame + static_cast<uint64_t>(*st.frames);
      }
      r.cv.notify_all();
      if (!WaitUntil(r, static_cast<double>(*st.frames) / 10.0 + 30.0,
                     [&] { return r.frame >= target; })) {
        return Fail("frame step did not complete");
      }
      return {};
    }

    case Verb::kResume: {
      {
        std::lock_guard lk(r.mu);
        r.paused = false;
        r.step_budget = 0;
      }
      r.cv.notify_all();
      return {};
    }

    case Verb::kExpect:
      return ExecuteExpect(r, st);

    case Verb::kExit:
      FinishAndExit(r, st.code, st.code == 0 ? "pass" : "fail", "exit step");

    case Verb::kCrash: {
      if (!r.scenario || !r.scenario->allow_crash) {
        return Fail("crash is refused unless the scenario sets allow_crash = true");
      }
      Event(r, "crash", "\"where\":\"" + JsonEscape(st.where) + "\"");
      REXLOG_WARN("harness: crashing on purpose ({})", st.where);
      rex::FlushLogging();
      {
        std::lock_guard lk(r.file_mu);
        r.events.flush();
        r.state_out.flush();
        r.log_out.flush();
      }
      if (st.where == "guest") {
        crash::ArmGuestFault();
        // The next pad poll faults on a guest thread; if the game stops
        // polling the pad, that never happens.
        WaitFrames(r, 300, 30.0);
        return Fail("the guest fault did not trigger within 300 frames (pad not polled?)");
      }
      crash::FaultNow();
      return Fail("the fault did not crash the process");
    }

    case Verb::kCapture: {
      if (!rex::ui::renderdoc::Attached()) {
        return Fail("RenderDoc is not attached (launch the game from RenderDoc, or preload it)");
      }
      std::error_code ec;
      fs::create_directories(r.dir / "captures", ec);
      const std::string tag = st.tag.empty() ? "capture" : SafeName(st.tag);
      const uint32_t before = rex::ui::renderdoc::NumCaptures();
      // Explicit bracket over `frames` game frames (default 1), aligned to
      // the guest frame boundary. TriggerCapture would wait for a present on
      // a window RenderDoc tracks, which it does not do on every window
      // system (Wayland under gamescope).
      const int64_t frames = st.frames ? std::max<int64_t>(1, *st.frames) : 1;
      if (!WaitFrames(r, 1, 30.0)) {
        return Fail("game stopped presenting frames");
      }
      rex::ui::renderdoc::StartFrameCapture((r.dir / "captures" / tag).string());
      const bool frames_ok = WaitFrames(r, frames, 60.0);
      const bool written = rex::ui::renderdoc::EndFrameCapture();
      if (!frames_ok) {
        return Fail("game stopped presenting frames during the capture");
      }
      if (!written) {
        return Fail(
            "RenderDoc did not write the capture (see its log in /tmp/RenderDoc, or "
            "%TEMP%\\RenderDoc on Windows)");
      }
      // Writing the file can take a few seconds on a handheld.
      const auto deadline = Clock::now() + std::chrono::seconds(60);
      while (rex::ui::renderdoc::NumCaptures() <= before) {
        if (Clock::now() > deadline || r.stop.load()) {
          return Fail("RenderDoc did not report the capture within 60 s");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      const std::string path = rex::ui::renderdoc::CapturePath(before);
      {
        std::lock_guard lk(r.mu);
        r.captures.push_back(path);
      }
      Outcome o;
      o.result_json = "\"file\":\"" + JsonEscape(path) + "\"";
      return o;
    }

    case Verb::kTeleport: {
      // A previous teleport, fall or respawn may still be finishing (presence
      // leaves gameplay during the fade): wait for free skate before issuing.
      constexpr double kReadyWaitS = 20.0;
      if (!WaitUntil(r, kReadyWaitS,
                     [&] { return r.state.presence == 1 && r.state.skater_valid; })) {
        return Fail("teleport needs gameplay (free skate with the skater in the world)");
      }
      // A streamed destination moves the skater seconds later. Arrival means
      // near the locator AND settled there (slow) for kSettleFrames frames in a
      // row. A freshly streamed district's collision can arrive after the
      // move: the skater then falls through the world. Falling well below the
      // locator after reaching it re-issues the same teleport (the district is
      // loaded by then), up to `retries` times within timeout_s.
      constexpr uint64_t kSettleFrames = 30;
      constexpr double kSettleSpeed = 2.0;
      // Many locators float a few metres above the ground (the skater drops
      // onto it), so arrival is horizontal distance within `tolerance` and
      // height between kFellBelow under and `tolerance` over the locator. A
      // fall through the world drops hundreds of metres.
      constexpr double kFellBelow = 15.0;
      const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                               std::chrono::duration<double>(st.timeout_s));
      game::TeleportResult tr;
      char target[96] = "[]";
      double dist = -1.0, speed = 0.0;
      bool arrived = false;
      int attempt = 0;
      for (; attempt <= st.retries; ++attempt) {
        const auto ready_deadline =
            Clock::now() + std::chrono::duration_cast<Clock::duration>(
                               std::chrono::duration<double>(kReadyWaitS));
        const uint64_t key = st.via_sequencer ? game::kForceSequencer : st.map_key;
        tr = game::Teleport(st.location, 15.0, key);
        while (!tr.ok && tr.busy && Clock::now() < ready_deadline && !r.stop.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          tr = game::Teleport(st.location, 15.0, key);
        }
        if (!tr.ok) {
          return Fail("teleport to '" + st.location + "': " + tr.error);
        }
        std::snprintf(target, sizeof(target), "[%.3f,%.3f,%.3f]", tr.target[0], tr.target[1],
                      tr.target[2]);
        Event(r, attempt ? "teleport_retry" : "teleport",
              "\"location\":\"" + JsonEscape(st.location) + "\",\"target\":" + target +
                  ",\"streamed\":" + (tr.streamed ? "true" : "false") +
                  ",\"path\":\"" + (tr.via_service ? "map" : "sequencer") + "\"" +
                  ",\"map_load\":" + (tr.map_load ? "true" : "false") + ",\"district\":\"" +
                  JsonEscape(tr.district) + "\",\"attempt\":" + std::to_string(attempt + 1));
        bool reached = false, fell = false;
        uint64_t settled_since = 0;
        const double left =
            std::max(0.5, std::chrono::duration<double>(deadline - Clock::now()).count());
        WaitUntil(r, left, [&] {
          if (!r.state.skater_valid) {
            settled_since = 0;
            return false;
          }
          const double dx = r.state.skater_pos[0] - tr.target[0];
          const double dy = r.state.skater_pos[1] - tr.target[1];
          const double dz = r.state.skater_pos[2] - tr.target[2];
          dist = std::sqrt(dx * dx + dy * dy + dz * dz);
          speed = r.state.skater_speed;
          const bool near = std::sqrt(dx * dx + dz * dz) <= st.tolerance &&
                            dy >= -kFellBelow && dy <= st.tolerance;
          if (near) {
            reached = true;
          }
          if (!st.settle) {
            arrived = reached;  // arrival is enough
            return arrived;
          }
          if (reached && dy < -kFellBelow) {
            fell = true;  // through the floor: retry
            return true;
          }
          if (!near || speed > kSettleSpeed) {
            settled_since = 0;
            return false;
          }
          if (!settled_since) {
            settled_since = r.state.frame;
          }
          arrived = r.state.frame - settled_since >= kSettleFrames;
          return arrived;
        });
        if (arrived || !fell || Clock::now() >= deadline) {
          break;
        }
      }
      if (!arrived) {
        char why[240];
        std::snprintf(why, sizeof(why),
                      "teleport to '%s' issued %d time(s), but the skater did not settle there: "
                      "%.1f m from %s at %.1f m/s after %.0f s",
                      st.location.c_str(), std::min(attempt + 1, st.retries + 1), dist, target,
                      speed, st.timeout_s);
        return Fail(why);
      }
      Outcome o;
      char extra[256];
      std::snprintf(extra, sizeof(extra),
                    "\"location\":\"%s\",\"target\":%s,\"distance\":%.2f,\"streamed\":%s,"
                    "\"attempts\":%d",
                    JsonEscape(st.location).c_str(), target, dist, tr.streamed ? "true" : "false",
                    attempt + 1);
      o.result_json = extra;
      return o;
    }

    case Verb::kRenderer: {
      const bool native = st.renderer == "native";
      if (!SetRenderer(native)) {
        return Fail("renderer: the switch to " + st.renderer +
                    " was refused (native layer not enabled at boot?)");
      }
      const uint64_t f0 = r.state.frame;
      const int64_t settle = st.frames.value_or(30);
      WaitUntil(r, 20.0, [&] { return r.state.frame >= f0 + static_cast<uint64_t>(settle); });
      Event(r, "renderer", "\"renderer\":\"" + st.renderer + "\"");
      Outcome o;
      o.result_json = "\"renderer\":\"" + st.renderer + "\"";
      return o;
    }

    case Verb::kPlayReel: {
      // Free skate first (the reel loads from there and returns there).
      if (!WaitUntil(r, 30.0, [&] { return r.state.presence == 1 && r.state.skater_valid; })) {
        return Fail("play_reel needs gameplay (free skate with the skater in the world)");
      }
      std::string err;
      game::ReelOptions opts;
      opts.from_marker = st.from == "marker";
      for (double v : st.stills) {
        opts.stills_s.push_back(static_cast<float>(v));
      }
      if (!game::StartReel(static_cast<int>(st.clip), opts, &err)) {
        return Fail("play_reel: " + err);
      }
      const std::string clip = "\"clip\":" + std::to_string(st.clip);
      Event(r, "reel_load", clip);
      const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                               std::chrono::duration<double>(st.timeout_s));
      auto left = [&] {
        return std::max(0.5, std::chrono::duration<double>(deadline - Clock::now()).count());
      };
      auto phase = [] { return game::GetReelStatus().phase; };
      using game::ReelPhase;
      WaitUntil(r, left(), [&] { return phase() != ReelPhase::kIdle && phase() != ReelPhase::kLoading; });
      game::ReelStatus s = game::GetReelStatus();
      if (s.phase == ReelPhase::kFailed || s.phase == ReelPhase::kIdle ||
          s.phase == ReelPhase::kLoading) {
        return Fail("play_reel clip " + std::to_string(st.clip) + ": " +
                    (s.error.empty() ? "did not start before timeout_s" : s.error));
      }
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    ",\"duration_s\":%.3f,\"from\":\"%s\",\"start_s\":%.3f,\"marker_s\":%.3f",
                    s.duration_s, st.from.c_str(), s.start_s, s.marker_s);
      Event(r, "reel_start", clip + buf);
      const uint64_t start_frame = r.state.frame;
      const auto start_time = Clock::now();
      // While playing: screenshots every `every_frames` frames (tag defaults to
      // "reel<clip>"), and at each still (the clock holds there): wait `settle`
      // frames, take "reel<clip>_t<seconds>", release the hold.
      const std::string tag = st.tag.empty() ? "reel" + std::to_string(st.clip) : st.tag;
      const bool periodic = st.every_frames_given && st.every_frames > 0;
      std::vector<std::string> shots;
      std::string stills_json;
      uint64_t next = start_frame + (periodic ? static_cast<uint64_t>(st.every_frames) : 0);
      int held_index = -2;
      uint64_t held_since = 0;
      while (phase() == ReelPhase::kPlaying && Clock::now() < deadline && !r.stop.load()) {
        const game::ReelStatus cur = game::GetReelStatus();
        if (cur.holding) {
          if (cur.hold_index != held_index) {
            held_index = cur.hold_index;
            held_since = r.state.frame;
          }
          if (r.state.frame >= held_since + static_cast<uint64_t>(st.settle_frames)) {
            // One shot per requested renderer (the F5 switch, then settle),
            // or one on the current renderer.
            const bool was_native = REXCVAR_GET(skate3_native_render_scene);
            std::vector<std::string> renderers = st.stills_renderers;
            if (renderers.empty()) {
              renderers.push_back("");
            }
            for (const std::string& rend : renderers) {
              if (!rend.empty()) {
                const bool want_native = rend == "native";
                if (!SetRenderer(want_native)) {
                  Event(r, "reel_still_skipped",
                        clip + ",\"renderer\":\"" + rend + "\",\"why\":\"switch refused\"");
                  continue;
                }
                const uint64_t f0 = r.state.frame;
                WaitUntil(r, 10.0, [&] {
                  return r.state.frame >= f0 + static_cast<uint64_t>(st.settle_frames);
                });
              }
              char still_tag[112];
              std::snprintf(still_tag, sizeof(still_tag), "%s_t%.2f%s%s", tag.c_str(), cur.t_s,
                            rend.empty() ? "" : "_", rend.c_str());
              std::string shot_err;
              std::vector<std::string> names;
              TakeScreenshots(r, still_tag, 1, 1, &names, &shot_err);
              const std::string file = names.empty() ? "" : JsonEscape(names[0]);
              char fields[240];
              std::snprintf(fields, sizeof(fields), "\"t\":%.3f,\"renderer\":\"%s\",\"file\":\"%s\"",
                            cur.t_s, rend.c_str(), file.c_str());
              stills_json += std::string(stills_json.empty() ? "" : ",") + "{" + fields + "}";
              Event(r, "reel_still", clip + "," + fields);
            }
            if (!st.stills_renderers.empty()) {
              SetRenderer(was_native);
            }
            game::ReleaseReelHold();
            next = r.state.frame + (periodic ? static_cast<uint64_t>(st.every_frames) : 0);
          }
        } else if (periodic && r.state.frame >= next) {
          std::string shot_err;
          std::vector<std::string> names;
          if (TakeScreenshots(r, tag, 1, 1, &names, &shot_err)) {
            shots.insert(shots.end(), names.begin(), names.end());
          }
          next = r.state.frame + static_cast<uint64_t>(st.every_frames);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      WaitUntil(r, left(), [&] { return phase() != ReelPhase::kPlaying; });
      s = game::GetReelStatus();
      const double wall = std::chrono::duration<double>(Clock::now() - start_time).count();
      std::snprintf(buf, sizeof(buf),
                    ",\"duration_s\":%.3f,\"played_s\":%.3f,\"editor_frames\":%llu,"
                    "\"frames\":%llu,\"wall_s\":%.3f",
                    s.duration_s, s.t_s, static_cast<unsigned long long>(s.frames),
                    static_cast<unsigned long long>(r.state.frame - start_frame), wall);
      const std::string played = buf;
      Event(r, "reel_stop", clip + played);
      if (s.phase == ReelPhase::kPlaying) {
        return Fail("play_reel clip " + std::to_string(st.clip) + ": still playing at timeout_s");
      }
      WaitUntil(r, left(), [&] { return phase() == ReelPhase::kDone || phase() == ReelPhase::kFailed; });
      s = game::GetReelStatus();
      if (s.phase != ReelPhase::kDone) {
        return Fail("play_reel clip " + std::to_string(st.clip) + ": " +
                    (s.error.empty() ? "did not return to free skate" : s.error));
      }
      Event(r, "reel_exit", clip);
      Outcome o;
      o.result_json = clip + played + ",\"stills\":[" + stills_json + "],\"shots\":[";
      for (size_t i = 0; i < shots.size(); ++i) {
        o.result_json += (i ? ",\"" : "\"") + JsonEscape(shots[i]) + "\"";
      }
      o.result_json += "]";
      return o;
    }

    case Verb::kQuery: {
      Outcome o;
      std::lock_guard lk(r.mu);
      if (st.what == "status") {
        o.result_json = "\"status\":{\"frame\":" + std::to_string(r.frame) +
                        ",\"poll\":" + std::to_string(r.poll) +
                        ",\"recording\":" + (r.recording ? "true" : "false") +
                        ",\"playing\":" + (r.play_active ? "true" : "false") +
                        ",\"paused\":" + (r.paused ? "true" : "false") + ",\"dir\":\"" +
                        JsonEscape(r.dir.generic_string()) + "\"}";
      } else {
        o.result_json = "\"state\":" + StateJson(r.state);
      }
      return o;
    }
  }
  return Fail("unhandled verb");
}

std::string StepJson(const Step& st) {
  std::string s = "\"verb\":\"" + std::string(VerbName(st.verb)) + "\"";
  if (st.index) {
    s += ",\"step\":" + std::to_string(st.index);
  }
  if (!st.id.empty()) {
    s += ",\"id\":\"" + JsonEscape(st.id) + "\"";
  }
  if (!st.tag.empty()) {
    s += ",\"tag\":\"" + JsonEscape(st.tag) + "\"";
  }
  if (!st.text.empty()) {
    s += ",\"text\":\"" + JsonEscape(st.text) + "\"";
  }
  return s;
}

// ------------------------------------------------------------ threads

void RunnerLoop(Run& r) {
  const Scenario& sc = *r.scenario;
  REXLOG_INFO("harness: scenario '{}' started ({} steps) -> {}", sc.name, sc.steps.size(),
              r.dir.string());
  for (const Step& st : sc.steps) {
    if (r.stop.load()) {
      return;
    }
    Event(r, "step", StepJson(st));
    REXLOG_INFO("harness: step {} {}", st.index, VerbName(st.verb));
    Outcome o = Execute(r, st);
    if (!o.ok) {
      const std::string msg = "step " + std::to_string(st.index) + " (" + VerbName(st.verb) +
                              "): " + o.error;
      REXLOG_ERROR("harness: {}", msg);
      {
        std::lock_guard lk(r.mu);
        r.failures.push_back(msg);
      }
      Event(r, "fail", StepJson(st) + ",\"error\":\"" + JsonEscape(o.error) + "\"");
      FinishAndExit(r, 1, "fail", msg);
    }
    Event(r, "done", StepJson(st) + (o.result_json.empty() ? "" : "," + o.result_json));
  }
  FinishAndExit(r, 0, "pass", "all steps completed");
}

void InboxLoop(Run& r) {
  const fs::path inbox = r.out_root / "inbox";
  const fs::path outbox = r.out_root / "outbox";
  std::error_code ec;
  fs::create_directories(inbox, ec);
  fs::create_directories(outbox, ec);
  while (!r.stop.load()) {
    std::vector<fs::path> files;
    for (auto it = fs::directory_iterator(inbox, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
      if (it->is_regular_file(ec) && it->path().extension() == ".toml") {
        files.push_back(it->path());
      }
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
      const std::string text = ReadFileText(file);
      fs::remove(file, ec);
      std::string error;
      auto st = ParseCommand(text, file.filename().string(), &error);
      std::string id = file.stem().string();
      Outcome o;
      if (!st) {
        o = Fail(error);
      } else {
        if (!st->id.empty()) {
          id = st->id;
        }
        Event(r, "command", StepJson(*st));
        o = Execute(r, *st);
        Event(r, o.ok ? "command_done" : "command_fail",
              StepJson(*st) + (o.ok ? "" : ",\"error\":\"" + JsonEscape(o.error) + "\""));
      }
      std::string reply = "{\"id\":\"" + JsonEscape(id) + "\",\"ok\":" +
                          (o.ok ? "true" : "false");
      if (!o.ok) {
        reply += ",\"error\":\"" + JsonEscape(o.error) + "\"";
      }
      if (!o.result_json.empty()) {
        reply += "," + o.result_json;
      }
      reply += "}\n";
      const fs::path tmp = outbox / (id + ".json.part");
      std::ofstream(tmp, std::ios::binary | std::ios::trunc) << reply;
      fs::rename(tmp, outbox / (id + ".json"), ec);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void WatchdogLoop(Run& r, double timeout_s) {
  const auto deadline = r.t0 + std::chrono::duration_cast<Clock::duration>(
                                   std::chrono::duration<double>(timeout_s));
  while (!r.stop.load()) {
    if (Clock::now() >= deadline) {
      {
        std::lock_guard lk(r.mu);
        r.failures.push_back("scenario timeout after " + std::to_string(timeout_s) + " s");
      }
      FinishAndExit(r, 2, "timeout", "scenario timeout_s exceeded");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

std::string Stamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

std::string SafeName(std::string s) {
  for (char& c : s) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      c = '_';
    }
  }
  return s.empty() ? "run" : s;
}

// ------------------------------------------------------------ pad hook

void OnPadPoll(uint8_t* base, uint32_t user, uint32_t state_addr, uint32_t result) {
  Run* r = g_run;
  if (!r || !g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  crash::MaybeGuestFault();  // `crash where = "guest"`: fault inside the guest's call
  if ((user & 0xFF) != 0 && (user & 0xFF) != 0xFF) {
    return;  // player one only
  }
  if (result != 0 || state_addr == 0) {
    return;
  }
  uint8_t raw[16];
  if (!native_scene::GuestTryCopy(raw, base + state_addr, sizeof(raw))) {
    return;
  }
  auto be16 = [&](int off) { return static_cast<uint16_t>((raw[off] << 8) | raw[off + 1]); };
  const uint32_t packet = (uint32_t(raw[0]) << 24) | (uint32_t(raw[1]) << 16) |
                          (uint32_t(raw[2]) << 8) | raw[3];
  PadState in;
  in.buttons = be16(4);
  in.lt = raw[6];
  in.rt = raw[7];
  in.lx = static_cast<int16_t>(be16(8));
  in.ly = static_cast<int16_t>(be16(10));
  in.rx = static_cast<int16_t>(be16(12));
  in.ry = static_cast<int16_t>(be16(14));

  PadState out = in;
  uint32_t bias;
  {
    std::lock_guard lk(r->mu);
    ++r->poll;
    if (r->play_active && r->play) {
      const uint64_t offset = r->poll - r->play_start_poll;
      if (r->poll > r->play_end_poll) {
        r->play_active = false;
      } else if (const PadState* p = PadAtPoll(*r->play, offset, &r->play_cursor)) {
        out = r->play_override ? *p : Overlay(out, *p);
      }
    }
    if (r->script_active) {
      if (r->frame >= r->script_until_frame) {
        r->script_active = false;
      } else {
        out = r->script_override ? r->script_pad : Overlay(out, r->script_pad);
      }
    }
    if (r->recording && (!r->rec_any || out != r->rec_last)) {
      RecRow row{r->poll - r->rec_start_poll, r->frame - r->rec_start_frame, out};
      r->rec_out << FormatRecRow(row);
      r->rec_last = out;
      r->rec_any = true;
      ++r->rec_rows;
    }
    if (out != r->last_out) {
      ++r->packet_bias;
      r->last_out = out;
    }
    bias = r->packet_bias;
  }
  r->cv.notify_all();
  if (out == in) {
    return;
  }
  const uint32_t pk = packet + bias;
  uint8_t w[16] = {
      uint8_t(pk >> 24),          uint8_t(pk >> 16),          uint8_t(pk >> 8),
      uint8_t(pk),                uint8_t(out.buttons >> 8),  uint8_t(out.buttons),
      out.lt,                     out.rt,                     uint8_t(uint16_t(out.lx) >> 8),
      uint8_t(uint16_t(out.lx)),  uint8_t(uint16_t(out.ly) >> 8), uint8_t(uint16_t(out.ly)),
      uint8_t(uint16_t(out.rx) >> 8), uint8_t(uint16_t(out.rx)), uint8_t(uint16_t(out.ry) >> 8),
      uint8_t(uint16_t(out.ry)),
  };
  std::memcpy(base + state_addr, w, sizeof(w));
}

}  // namespace

// ------------------------------------------------------------ public API

bool Enabled() {
  return g_enabled.load(std::memory_order_relaxed);
}

void Prepare() {
  const std::string scenario_path = REXCVAR_GET(skate3_harness_scenario);
  if (scenario_path.empty() && !REXCVAR_GET(skate3_harness)) {
    return;
  }
  auto* r = new Run();
  r->out_root = fs::absolute(fs::path(std::string(REXCVAR_GET(skate3_harness_out))));
  std::string name = "live";
  if (!scenario_path.empty()) {
    std::string error;
    r->scenario = LoadScenario(scenario_path, &error);
    if (!r->scenario) {
      REXLOG_ERROR("harness: {}", error);
      std::fprintf(stderr, "harness: %s\n", error.c_str());
      rex::FlushLogging();
      std::_Exit(3);
    }
    name = r->scenario->name;
    r->state_every = r->scenario->state_every_frames;
    if (r->scenario->boot_demo) {
      rex::cvar::SetFlagByName("skate3_demo_path", "true");
    }
    for (const auto& [k, v] : r->scenario->cvars) {
      if (!rex::cvar::SetFlagByName(k, v)) {
        REXLOG_WARN("harness: scenario cvar {} = {} was rejected", k, v);
      }
    }
  }
  r->dir = r->out_root / (Stamp() + "_" + SafeName(name));
  std::error_code ec;
  fs::create_directories(r->dir, ec);
  r->events.open(r->dir / "events.jsonl", std::ios::binary | std::ios::trunc);
  r->state_out.open(r->dir / "state.jsonl", std::ios::binary | std::ios::trunc);
  r->log_out.open(r->dir / "log.txt", std::ios::binary | std::ios::trunc);
  std::ofstream(r->out_root / "current_run.txt", std::ios::trunc) << r->dir.generic_string()
                                                                   << "\n";
  // Which build this is, written now: a crashed run never gets a result.json,
  // and run_scenario.py needs the platform and config to symbolize its frames.
  std::ofstream(r->dir / "build.json", std::ios::trunc)
      << "{\"version\": \"" << SKATE3_VERSION_STRING << "\", \"platform\": \""
      << SKATE3_BUILD_PLATFORM << "\", \"build_config\": \"" << SKATE3_BUILD_CONFIG << "\"}\n";
  crash::Install(r->dir);
  g_run = r;
  g_tap = std::make_shared<LogTap>(r);
  rex::AddSink(g_tap);
  g_enabled.store(true, std::memory_order_release);
  REXLOG_INFO("harness: enabled ({}), run folder {}", r->scenario ? "scenario" : "live",
              r->dir.string());
  if (r->scenario && r->scenario->vulkan_validation &&
      rex::cvar::GetFlagByName("vulkan_validation_enabled") != "true" &&
      rex::cvar::GetFlagByName("d3d12_debug") != "true") {
    REXLOG_WARN(
        "harness: the scenario asks for validation, but the graphics device was created "
        "without it; pass --vulkan_validation_enabled=true --vulkan_log_debug_messages=true "
        "(Vulkan) or --d3d12_debug=true (D3D12); run_scenario.py does");
  }
  Event(*r, "prepare",
        "\"scenario\":\"" + JsonEscape(r->scenario ? r->scenario->path : "") + "\"");
}

void Start(CaptureFn capture, CloseFn close) {
  Run* r = g_run;
  if (!r) {
    return;
  }
  r->capture = std::move(capture);
  r->close = std::move(close);
  r->encoder = std::thread([r] { EncoderLoop(*r); });
  r->inbox = std::thread([r] { InboxLoop(*r); });
  if (r->scenario) {
    r->watchdog = std::thread([r] { WatchdogLoop(*r, r->scenario->timeout_s); });
    r->runner = std::thread([r] { RunnerLoop(*r); });
  }
}

void Shutdown() {
  Run* r = g_run;
  if (!r) {
    return;
  }
  g_enabled.store(false, std::memory_order_release);
  StopRecording(*r);
  r->stop.store(true);
  {
    std::lock_guard lk(r->mu);
    r->paused = false;
  }
  r->cv.notify_all();
  r->shot_cv.notify_all();
  // After FinishAndExit asked for a normal close, the thread that called it
  // (runner or watchdog) never returns: detach instead of joining. Run is
  // never freed, so a detached thread can't touch freed memory.
  const bool finished = r->finished.load();
  for (std::thread* t : {&r->runner, &r->inbox, &r->watchdog, &r->encoder}) {
    if (t->joinable()) {
      // The runner may sit in a long wait; it checks stop every 50 ms.
      if (finished && (t == &r->runner || t == &r->watchdog)) {
        t->detach();
      } else {
        t->join();
      }
    }
  }
  if (g_tap) {
    rex::RemoveSink(g_tap);
  }
  // g_run is left allocated: guest threads may still be inside a hook.
}

void OnSkaterEntity(uint32_t entity) {
  Run* r = g_run;
  if (!r || !g_enabled.load(std::memory_order_relaxed) || entity == 0) {
    return;
  }
  const uint64_t frame = r->frame_atomic.load(std::memory_order_relaxed);
  std::lock_guard lk(r->skater_mu);
  for (auto& [e, seen] : r->skaters_seen) {
    if (e == entity) {
      seen = frame;
      return;
    }
  }
  if (r->skaters_seen.size() < 64) {
    r->skaters_seen.emplace_back(entity, frame);
  }
}

void OnFrame(uint8_t* base) {
  Run* r = g_run;
  if (!r || !g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  const auto now = Clock::now();

  // Sample state outside the run lock (guest reads can be slow).
  GameState s;
  s.presence = rex::kernel::guest_presence::GameplayContextValue();
  uint32_t mgr = 0, beg = 0, end = 0;
  if (ReadU32(base, kFrontEndManagerPtr, &mgr) && mgr && ReadU32(base, mgr + kFeStackBegin, &beg) &&
      ReadU32(base, mgr + kFeStackEnd, &end) && end > beg && (end - beg) % kFeRecordSize == 0 &&
      (end - beg) / kFeRecordSize < 64) {
    s.fe_depth = (end - beg) / kFeRecordSize;
    for (uint32_t a = beg; a < end; a += kFeRecordSize) {
      uint32_t id = 0, second = 0;
      if (!ReadU32(base, a, &id) || !ReadU32(base, a + 4, &second)) {
        break;
      }
      s.fe_stack.emplace_back(static_cast<int32_t>(id), static_cast<int32_t>(second));
    }
    if (!s.fe_stack.empty()) {
      s.fe_top = s.fe_stack.back().first;
    }
  }
  if (const uint32_t vc = native_scene::MainViewCamera()) {
    float v[16];
    if (ReadF32x16(base, vc + 0x20, v)) {
      // cam = -t * R^T (row-vector view), as in BuildFrameScene.
      for (int j = 0; j < 3; ++j) {
        s.cam_pos[j] = -(v[12] * v[j * 4 + 0] + v[13] * v[j * 4 + 1] + v[14] * v[j * 4 + 2]);
      }
      s.cam_valid = std::isfinite(s.cam_pos[0]) && std::isfinite(s.cam_pos[1]) &&
                    std::isfinite(s.cam_pos[2]);
    }
  }
  std::vector<uint32_t> skaters;
  {
    const uint64_t frame = r->frame_atomic.load(std::memory_order_relaxed);
    std::lock_guard lk(r->skater_mu);
    auto& seen = r->skaters_seen;
    seen.erase(std::remove_if(seen.begin(), seen.end(),
                              [&](const auto& p) { return frame - p.second > kSkaterKeepFrames; }),
               seen.end());
    for (const auto& p : seen) {
      skaters.push_back(p.first);
    }
  }
  // The skater tracked last frame stays a candidate while its transform
  // reads valid: binding is intermittent, its matrix is not.
  if (r->last_skater &&
      std::find(skaters.begin(), skaters.end(), r->last_skater) == skaters.end()) {
    skaters.push_back(r->last_skater);
  }
  // The player's skater: the one the camera follows (closest to it), else
  // the one tracked last frame, else the first bound.
  uint32_t chosen = 0;
  float chosen_pos[3] = {};
  float best = 0.0f;
  s.skater_candidates = static_cast<uint32_t>(skaters.size());
  for (uint32_t e : skaters) {
    float m[16] = {};
    const bool read_ok = ReadF32x16(base, e + kEntityMatLtoW, m);
    // Frontend and stand-in skaters sit at the origin with an identity
    // transform; the player's never does.
    const bool at_origin =
        std::fabs(m[12]) < 1e-3f && std::fabs(m[13]) < 1e-3f && std::fabs(m[14]) < 1e-3f;
    if (!read_ok || at_origin || !std::isfinite(m[12]) ||
        !std::isfinite(m[13]) || !std::isfinite(m[14]) || std::fabs(m[12]) > 1e6f ||
        std::fabs(m[13]) > 1e6f || std::fabs(m[14]) > 1e6f) {
      continue;
    }
    float score;
    if (s.cam_valid) {
      const float dx = m[12] - s.cam_pos[0], dy = m[13] - s.cam_pos[1],
                  dz = m[14] - s.cam_pos[2];
      score = dx * dx + dy * dy + dz * dz;
    } else {
      score = (e == r->last_skater) ? 0.0f : 1.0f;
    }
    if (!chosen || score < best) {
      chosen = e;
      best = score;
      chosen_pos[0] = m[12];
      chosen_pos[1] = m[13];
      chosen_pos[2] = m[14];
    }
  }

  bool write_state = false;
  std::string state_line;
  std::string fe_event;
  const char* replay_event = nullptr;
  const char* movie_event = nullptr;
  {
    std::unique_lock lk(r->mu);
    ++r->frame;
    r->frame_atomic.store(r->frame, std::memory_order_relaxed);
    const double dt = std::chrono::duration<double>(now - r->last_frame_time).count();
    r->last_frame_time = now;
    s.frame = r->frame;
    s.t = std::chrono::duration<double>(now - r->t0).count();
    s.fps = dt > 0.0 ? (r->state.fps * 0.9 + (1.0 / dt) * 0.1) : r->state.fps;
    if (chosen) {
      s.skater = chosen;
      s.skater_valid = true;
      std::memcpy(s.skater_pos, chosen_pos, sizeof(chosen_pos));
      if (r->have_last_pos && chosen == r->last_skater) {
        const double pdt = std::chrono::duration<double>(now - r->last_pos_time).count();
        if (pdt > 0.0) {
          const double dx = chosen_pos[0] - r->last_pos[0];
          const double dy = chosen_pos[1] - r->last_pos[1];
          const double dz = chosen_pos[2] - r->last_pos[2];
          const double inst = std::sqrt(dx * dx + dy * dy + dz * dz) / pdt;
          const double hinst = std::sqrt(dx * dx + dz * dz) / pdt;
          // Light smoothing: frame pacing jitter, not physics, dominates.
          s.skater_speed = r->state.skater_speed * 0.5 + inst * 0.5;
          s.skater_hspeed = r->state.skater_hspeed * 0.5 + hinst * 0.5;
        }
      }
      r->last_skater = chosen;
      std::memcpy(r->last_pos, chosen_pos, sizeof(chosen_pos));
      r->last_pos_time = now;
      r->have_last_pos = true;
    } else {
      s.skater_speed = 0.0;
      s.skater_hspeed = 0.0;
    }
    s.paused = r->paused;
    s.replay_active = r->frame - g_replay_tick_frame.load(std::memory_order_relaxed) <=
                          kHeartbeatFrames &&
                      g_replay_tick_frame.load(std::memory_order_relaxed) != 0;
    s.movie_active = r->frame - g_movie_tick_frame.load(std::memory_order_relaxed) <=
                         kHeartbeatFrames &&
                     g_movie_tick_frame.load(std::memory_order_relaxed) != 0;
    if (s.fe_stack != r->state.fe_stack) {
      fe_event = "\"stack\":" + FeStackJson(s.fe_stack) + ",\"from\":" +
                 FeStackJson(r->state.fe_stack);
    }
    if (s.replay_active != r->state.replay_active) {
      replay_event = s.replay_active ? "replay_start" : "replay_stop";
    }
    if (s.movie_active != r->state.movie_active) {
      movie_event = s.movie_active ? "movie_start" : "movie_stop";
    }
    r->state = s;
    if (r->state_every > 0 && r->frame % static_cast<uint64_t>(r->state_every) == 0) {
      write_state = true;
      state_line = StateJson(s);
    }
    r->cv.notify_all();

    // Pause / frame stepping: hold the guest's frame boundary.
    while (r->paused && r->step_budget <= 0 && !r->stop.load()) {
      r->state.paused = true;
      r->cv.wait_for(lk, std::chrono::milliseconds(100));
    }
    if (r->paused && r->step_budget > 0) {
      --r->step_budget;
    }
  }
  if (write_state) {
    std::lock_guard lk(r->file_mu);
    r->state_out << state_line << "\n";
  }
  // Transitions (after the run lock: Event takes it).
  if (!fe_event.empty()) {
    Event(*r, "fe", fe_event);
  }
  if (replay_event) {
    Event(*r, replay_event, {});
  }
  if (movie_event) {
    Event(*r, movie_event, {});
  }
}

void OnReplayTick() {
  if (Run* r = g_run; r && g_enabled.load(std::memory_order_relaxed)) {
    g_replay_tick_frame.store(r->frame_atomic.load(std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
  }
}

void OnMovieTick() {
  if (Run* r = g_run; r && g_enabled.load(std::memory_order_relaxed)) {
    g_movie_tick_frame.store(r->frame_atomic.load(std::memory_order_relaxed) + 1,
                             std::memory_order_relaxed);
  }
}

void Note(const char* kind, const std::string& fields_json) {
  if (Run* r = g_run; r && g_enabled.load(std::memory_order_relaxed)) {
    Event(*r, kind, fields_json);
  }
}

Status CurrentStatus() {
  Status st;
  Run* r = g_run;
  if (!r || !g_enabled.load(std::memory_order_relaxed)) {
    return st;
  }
  st.enabled = true;
  std::lock_guard lk(r->mu);
  st.recording = r->recording;
  st.playing = r->play_active;
  st.scripted = r->script_active;
  st.paused = r->paused;
  if (r->recording) {
    st.record_seconds = static_cast<double>(r->frame - r->rec_start_frame) / 60.0;
  }
  return st;
}

void ToggleRecording() {
  Run* r = g_run;
  if (!r || !g_enabled.load()) {
    return;
  }
  bool recording;
  {
    std::lock_guard lk(r->mu);
    recording = r->recording;
  }
  if (recording) {
    const std::string name = StopRecording(*r);
    REXLOG_INFO("harness: recording stopped -> {}", name);
    Event(*r, "record_stop", "\"file\":\"" + JsonEscape(name) + "\"");
  } else {
    StartRecording(*r, {}, "bind");
    std::string name;
    {
      std::lock_guard lk(r->mu);
      name = r->rec_path.filename().string();
    }
    REXLOG_INFO("harness: recording started -> {}", name);
    Event(*r, "record_start", "\"file\":\"" + JsonEscape(name) + "\"");
  }
}

}  // namespace skate3::harness

// XInputGetState(user, state) -> XamInputGetState(user, 1, state): the
// game's only path to the pad. The harness reads (recording) and rewrites
// (scripted input, playback) the state the game receives, per poll.
extern "C" REX_FUNC(sub_82EE22C0) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_82EE22C0(ctx, base);
  skate3::harness::OnPadPoll(base, user, state, ctx.r3.u32);
}

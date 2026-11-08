#include "harness/harness_script.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <toml++/toml.hpp>

namespace skate3::harness {
namespace {

struct VerbEntry {
  const char* name;
  Verb verb;
};
constexpr VerbEntry kVerbs[] = {
    {"wait", Verb::kWait},         {"input", Verb::kInput},
    {"play", Verb::kPlay},         {"record", Verb::kRecord},
    {"cvar", Verb::kCvar},         {"screenshot", Verb::kScreenshot},
    {"state", Verb::kState},       {"mark", Verb::kMark},
    {"snapshot", Verb::kSnapshot}, {"pause", Verb::kPause},
    {"step", Verb::kStep},         {"resume", Verb::kResume},
    {"expect", Verb::kExpect},     {"exit", Verb::kExit},
    {"query", Verb::kQuery},       {"capture", Verb::kCapture},
    {"crash", Verb::kCrash},       {"teleport", Verb::kTeleport},
    {"play_reel", Verb::kPlayReel},
};

// TOML scalar -> the string form rex::cvar::SetFlagByName takes.
std::optional<std::string> ScalarString(const toml::node& n) {
  if (n.is_boolean()) {
    return *n.value<bool>() ? "true" : "false";
  }
  if (n.is_integer()) {
    return std::to_string(*n.value<int64_t>());
  }
  if (n.is_floating_point()) {
    std::ostringstream os;
    os << *n.value<double>();
    return os.str();
  }
  if (auto v = n.value<std::string>()) {
    return *v;
  }
  return std::nullopt;
}

std::optional<double> Number(const toml::table& t, std::string_view key) {
  const toml::node* n = t.get(key);
  if (!n) {
    return std::nullopt;
  }
  if (n->is_integer()) {
    return static_cast<double>(*n->value<int64_t>());
  }
  if (n->is_floating_point()) {
    return *n->value<double>();
  }
  return std::nullopt;
}

std::optional<int64_t> Integer(const toml::table& t, std::string_view key) {
  if (auto d = Number(t, key)) {
    return static_cast<int64_t>(*d);
  }
  return std::nullopt;
}

bool ParseStepTable(const toml::table& t, Step* out, std::string* error) {
  Step s;
  const auto verb_name = t["do"].value<std::string>();
  if (!verb_name) {
    *error = "missing do = \"<verb>\"";
    return false;
  }
  bool found = false;
  for (const auto& v : kVerbs) {
    if (*verb_name == v.name) {
      s.verb = v.verb;
      found = true;
    }
  }
  if (!found) {
    *error = "unknown verb '" + *verb_name + "'";
    return false;
  }

  s.id = t["id"].value_or(std::string());
  s.gameplay = t["gameplay"].value_or(false);
  s.frames = Integer(t, "frames");
  s.ms = Integer(t, "ms");
  s.fe_screen = Integer(t, "fe_screen");
  s.presence = Integer(t, "presence");
  s.speed_gt = Number(t, "speed_gt");
  s.speed_lt = Number(t, "speed_lt");
  s.log = t["log"].value_or(std::string());
  s.no_log = t["no_log"].value_or(std::string());
  s.timeout_s = Number(t, "timeout_s").value_or(120.0);

  if (auto b = t["buttons"].value<std::string>()) {
    std::string bad;
    auto mask = ParseButtons(*b, &bad);
    if (!mask) {
      *error = "unknown button '" + bad + "'";
      return false;
    }
    s.pad.buttons = *mask;
  }
  s.pad.lt = TriggerFromUnit(Number(t, "lt").value_or(0.0));
  s.pad.rt = TriggerFromUnit(Number(t, "rt").value_or(0.0));
  s.pad.lx = AxisFromUnit(Number(t, "lx").value_or(0.0));
  s.pad.ly = AxisFromUnit(Number(t, "ly").value_or(0.0));
  s.pad.rx = AxisFromUnit(Number(t, "rx").value_or(0.0));
  s.pad.ry = AxisFromUnit(Number(t, "ry").value_or(0.0));
  const std::string mode = t["mode"].value_or(std::string("override"));
  if (mode != "override" && mode != "overlay") {
    *error = "mode must be \"override\" or \"overlay\"";
    return false;
  }
  s.override_pad = mode == "override";
  s.async = t["async"].value_or(false);

  s.file = t["file"].value_or(std::string());
  s.start = t["start"].value_or(true);
  s.note = t["note"].value_or(std::string());

  if (const toml::table* set = t["set"].as_table()) {
    for (const auto& [k, v] : *set) {
      auto str = ScalarString(v);
      if (!str) {
        *error = "cvar '" + std::string(k.str()) + "' must be a scalar";
        return false;
      }
      s.set.emplace_back(std::string(k.str()), *str);
    }
  }

  s.tag = t["tag"].value_or(std::string());
  s.count = std::max<int64_t>(1, Integer(t, "count").value_or(1));
  if (auto e = Integer(t, "every_frames")) {
    s.every_frames = std::max<int64_t>(0, *e);
    s.every_frames_given = true;
  }
  s.text = t["text"].value_or(std::string());
  s.code = static_cast<int>(Integer(t, "code").value_or(0));
  s.what = t["what"].value_or(std::string("state"));
  s.where = t["where"].value_or(std::string("runner"));
  s.location = t["location"].value_or(std::string());
  s.tolerance = Number(t, "tolerance").value_or(5.0);
  s.settle = t["settle"].value_or(true);
  s.retries = static_cast<int>(std::max<int64_t>(0, Integer(t, "retries").value_or(3)));
  const std::string tpath = t["path"].value_or(std::string("map"));
  if (tpath != "map" && tpath != "sequencer") {
    *error = "path must be \"map\" (default) or \"sequencer\"";
    return false;
  }
  s.via_sequencer = tpath == "sequencer";
  if (auto mk = t["map_key"].value<std::string>()) {
    try {
      s.map_key = std::stoull(*mk, nullptr, 16);
    } catch (const std::exception&) {
      *error = "map_key must be a hex string (e.g. \"A8B1F47090679026\")";
      return false;
    }
  }

  s.clip = Integer(t, "clip").value_or(0);

  // Per-verb requirements.
  switch (s.verb) {
    case Verb::kPlayReel:
      if (s.clip < 1) {
        *error = "play_reel needs clip = N (1-based saved replay, as listed in My Replays)";
        return false;
      }
      if (!t.get("timeout_s")) {
        s.timeout_s = 600.0;
      }
      break;
    case Verb::kTeleport:
      if (s.location.empty()) {
        *error = "teleport needs location = \"<locator name>\" (e.g. \"Z_UN_ClockTower\")";
        return false;
      }
      if (!t.get("timeout_s")) {
        s.timeout_s = 60.0;
      }
      break;
    case Verb::kCrash:
      if (s.where != "runner" && s.where != "guest") {
        *error = "crash needs where = \"runner\" or \"guest\"";
        return false;
      }
      break;
    case Verb::kWait:
      if (!s.gameplay && !s.frames && !s.ms && !s.fe_screen && !s.presence && s.log.empty() &&
          !s.speed_gt && !s.speed_lt) {
        *error = "wait needs one of gameplay, frames, ms, fe_screen, presence, log, speed_gt, "
                 "speed_lt";
        return false;
      }
      break;
    case Verb::kInput:
      if (!s.frames || *s.frames <= 0) {
        *error = "input needs frames = N (how long to hold)";
        return false;
      }
      break;
    case Verb::kPlay:
      if (s.file.empty()) {
        *error = "play needs file = \"<recording>.s3rec\"";
        return false;
      }
      break;
    case Verb::kCvar:
      if (s.set.empty()) {
        *error = "cvar needs a [step.set] table (or set = { name = value })";
        return false;
      }
      break;
    case Verb::kStep:
      if (!s.frames || *s.frames <= 0) {
        *error = "step needs frames = N";
        return false;
      }
      break;
    case Verb::kExpect:
      if (s.log.empty() && s.no_log.empty() && !s.speed_gt && !s.speed_lt && !s.presence) {
        *error = "expect needs one of log, no_log, speed_gt, speed_lt, presence";
        return false;
      }
      break;
    default:
      break;
  }
  *out = std::move(s);
  return true;
}

}  // namespace

const char* VerbName(Verb v) {
  for (const auto& e : kVerbs) {
    if (e.verb == v) {
      return e.name;
    }
  }
  return "?";
}

std::optional<Scenario> LoadScenario(const std::string& path, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = path + ": cannot open";
    return std::nullopt;
  }
  std::stringstream buf;
  buf << in.rdbuf();
  toml::table root;
  try {
    root = toml::parse(buf.str(), path);
  } catch (const toml::parse_error& e) {
    std::ostringstream os;
    os << path << ": " << e.description() << " (line " << e.source().begin.line << ")";
    *error = os.str();
    return std::nullopt;
  }
  Scenario sc;
  sc.path = path;
  sc.name = root["name"].value_or(std::string("scenario"));
  sc.timeout_s = Number(root, "timeout_s").value_or(600.0);
  const std::string boot = root["boot"].value_or(std::string("demo"));
  if (boot != "demo" && boot != "none") {
    *error = path + ": boot must be \"demo\" or \"none\"";
    return std::nullopt;
  }
  sc.boot_demo = boot == "demo";
  sc.state_every_frames = std::max<int64_t>(0, Integer(root, "state_every_frames").value_or(10));
  sc.vulkan_validation = root["vulkan_validation"].value_or(false);
  sc.allow_crash = root["allow_crash"].value_or(false);
  if (const toml::table* cv = root["cvars"].as_table()) {
    for (const auto& [k, v] : *cv) {
      auto str = ScalarString(v);
      if (!str) {
        *error = path + ": [cvars] " + std::string(k.str()) + " must be a scalar";
        return std::nullopt;
      }
      sc.cvars.emplace_back(std::string(k.str()), *str);
    }
  }
  if (const toml::array* steps = root["step"].as_array()) {
    size_t i = 0;
    for (const auto& node : *steps) {
      ++i;
      const toml::table* t = node.as_table();
      if (!t) {
        *error = path + ": step " + std::to_string(i) + ": not a table";
        return std::nullopt;
      }
      Step s;
      std::string why;
      if (!ParseStepTable(*t, &s, &why)) {
        *error = path + ": step " + std::to_string(i) + ": " + why;
        return std::nullopt;
      }
      s.index = i;
      sc.steps.push_back(std::move(s));
    }
  }
  // start_spot: teleport there right after the first gameplay wait, so every
  // target (ab) starts at the same place instead of the save's last location.
  sc.start_spot = root["start_spot"].value_or(std::string());
  if (!sc.start_spot.empty()) {
    auto it = std::find_if(sc.steps.begin(), sc.steps.end(),
                           [](const Step& s) { return s.verb == Verb::kWait && s.gameplay; });
    if (it == sc.steps.end()) {
      *error = path + ": start_spot needs a `wait` step with gameplay = true to follow";
      return std::nullopt;
    }
    Step tp;
    tp.verb = Verb::kTeleport;
    tp.location = sc.start_spot;
    tp.timeout_s = 120.0;  // a map load (Industrial) takes ~11 s on its own
    sc.steps.insert(it + 1, std::move(tp));
    for (size_t k = 0; k < sc.steps.size(); ++k) {
      sc.steps[k].index = k + 1;
    }
  }
  return sc;
}

std::optional<Step> ParseCommand(const std::string& text, const std::string& source,
                                 std::string* error) {
  toml::table root;
  try {
    root = toml::parse(text, source);
  } catch (const toml::parse_error& e) {
    *error = std::string(e.description());
    return std::nullopt;
  }
  Step s;
  if (!ParseStepTable(root, &s, error)) {
    return std::nullopt;
  }
  return s;
}

}  // namespace skate3::harness

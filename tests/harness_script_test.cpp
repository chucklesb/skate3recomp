// Unit test for src/harness/harness_script.cpp: the teleport step's forms
// (locator, coordinates, both) and their validation, through the live-command
// parser (scenario steps use the same per-step parser), and play_reel speed.

#include "harness/harness_script.h"

#include <string>

#include "test_check.h"

using namespace skate3::harness;

int main() {
  std::string error;

  // Locator only (the existing form).
  auto s = ParseCommand(R"(do = "teleport"
location = "Z_UN_ClockTower")",
                        "test", &error);
  CHECK(s.has_value());
  CHECK(s && s->verb == Verb::kTeleport && s->location == "Z_UN_ClockTower" && !s->has_pos);

  // Coordinates with a heading.
  s = ParseCommand(R"(do = "teleport"
pos = [-176.189, 49.557, -150.73]
yaw = -35.5)",
                   "test", &error);
  CHECK(s.has_value());
  CHECK(s && s->has_pos && s->location.empty());
  CHECK(s && s->pos[0] == -176.189f && s->pos[1] == 49.557f && s->pos[2] == -150.73f);
  CHECK(s && s->yaw && *s->yaw == -35.5);

  // Integers are fine; yaw is optional.
  s = ParseCommand("do = \"teleport\"\npos = [1, 2, 3]", "test", &error);
  CHECK(s && s->has_pos && s->pos[2] == 3.0f && !s->yaw);

  // Both: locator first (loads its district), then the coordinates.
  s = ParseCommand("do = \"teleport\"\nlocation = \"Z_DT_Tresoutta\"\npos = [1, 2, 3]",
                   "test", &error);
  CHECK(s && s->has_pos && s->location == "Z_DT_Tresoutta");

  // hop (pos without location only).
  s = ParseCommand("do = \"teleport\"\npos = [1, 2, 3]\nhop = false", "test", &error);
  CHECK(s && s->hop && !*s->hop);
  CHECK(!ParseCommand("do = \"teleport\"\nlocation = \"Z_DT_Tresoutta\"\npos = [1, 2, 3]\n"
                      "hop = true",
                      "test", &error)
             .has_value());

  // Errors.
  CHECK(!ParseCommand("do = \"teleport\"", "test", &error).has_value());
  CHECK(!ParseCommand("do = \"teleport\"\npos = [1, 2]", "test", &error).has_value());
  CHECK(!ParseCommand("do = \"teleport\"\npos = [1, \"a\", 3]", "test", &error).has_value());
  CHECK(!ParseCommand("do = \"teleport\"\nlocation = \"Z_UN_ClockTower\"\nyaw = 10", "test",
                      &error)
             .has_value());

  // play_reel speed: default 1, quarter speed, out of range rejected.
  s = ParseCommand("do = \"play_reel\"\nclip = 2", "test", &error);
  CHECK(s && s->verb == Verb::kPlayReel && s->speed == 1.0);
  s = ParseCommand("do = \"play_reel\"\nclip = 2\nspeed = 0.25", "test", &error);
  CHECK(s && s->speed == 0.25);
  s = ParseCommand("do = \"play_reel\"\nclip = 2\nspeed = 2", "test", &error);
  CHECK(s && s->speed == 2.0);
  CHECK(!ParseCommand("do = \"play_reel\"\nclip = 2\nspeed = 0", "test", &error).has_value());
  CHECK(!ParseCommand("do = \"play_reel\"\nclip = 2\nspeed = 5", "test", &error).has_value());

  return skate3_test::TestExit("PASS: harness_script_test");
}

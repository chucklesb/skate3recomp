#pragma once
// RE-7 replacement framework: native code in place of recompiled guest
// functions, verified in shadow mode first.
//
// A replacement is a Family: a name, a mode (off | shadow | on, from the
// family's own cvar), and a Compute function that fills Writes, the exact
// guest memory the recompiled function would write (rows of big-endian
// words at guest addresses). Run() does the rest:
//  - off: the recompiled function;
//  - shadow: Compute, the recompiled function, then a byte compare of every
//    row (exact / diff counters, the first differences logged);
//  - on: Compute and write; the recompiled function only when Compute
//    refuses (bad inputs, an unsupported case).
// skate3_native_replace_all = default | off | shadow overrides every family
// (A/B runs, bisecting a regression). One `replace:` line per log interval
// lists each family's counters (tools/harness/replace_report.py).
#include <atomic>
#include <cstdint>
#include <vector>

#include <rex/ppc/context.h>

namespace skate3::native_replace {

// The guest writes a Compute produces: rows of row_words big-endian words.
struct Writes {
  uint32_t row_words = 12;
  std::vector<uint32_t> addr;   // guest address of each row
  std::vector<uint32_t> words;  // row_words words per row, big-endian
  void Reset(uint32_t rw) {
    row_words = rw;
    addr.clear();
    words.clear();
  }
  // Appends one row; returns its words to fill.
  uint32_t* Add(uint32_t guest_addr) {
    addr.push_back(guest_addr);
    words.resize(words.size() + row_words);
    return words.data() + words.size() - row_words;
  }
};

using PPCFn = void (*)(PPCContext&, uint8_t*);
// Fills `out` for this call (registers as the recompiled function receives
// them). Returns false to let the recompiled function run (and count `bad`).
using ComputeFn = bool (*)(PPCContext& ctx, uint8_t* base, Writes* out);
// 0 off, 1 shadow, 2 on: the family's own setting.
using ModeFn = int (*)();

struct Family {
  const char* name;
  ModeFn mode;
  ComputeFn compute;
  // Window counters (exchanged to 0 at each stats line).
  std::atomic<uint64_t> calls{0}, exact{0}, diff{0}, native{0}, bad{0}, rows{0};
  int id = -1;  // assigned by Register
};

// Once per family (static storage), before its first Run.
void Register(Family* f);
// The mode after the global override.
int ModeOf(const Family& f);
// The hook body: see the header comment. `orig` is the __imp__ function.
void Run(Family& f, PPCContext& ctx, uint8_t* base, PPCFn orig);
// Frame tail (render thread): re-reads the global override; the stats line.
void OnFrameBuilt();

}  // namespace skate3::native_replace

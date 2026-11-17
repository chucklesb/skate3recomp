#pragma once
// Guest call counts (build option SKATE3_COUNT_GUEST_CALLS; see
// harness_callcount_hook.h). The harness step `callcount` op="reset" zeroes
// them, op="dump" writes call_counts.tsv (name, calls; nonzero, most first)
// into the run folder. tools/re/leaves.py rank <run> groups them by class.
#include <filesystem>
#include <string>

namespace skate3::callcount {

// Whether this build counts calls.
bool Enabled();
void Reset();
// Writes dir/call_counts.tsv; returns the result JSON fields
// ("file":...,"functions":N,"calls":N), or "" with *error set.
std::string Dump(const std::filesystem::path& dir, std::string* error);

}  // namespace skate3::callcount

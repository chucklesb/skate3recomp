#!/bin/bash
# A/B a cvar across whole scenario runs: RUNS runs per value in alternating
# order (A B B A A B ...), each printing status, frame times, the guest CPU
# per frame of each profiled thread (when the scenario has a `profile` step)
# and the summed key=value counters of the log lines that start with PREFIX
# (e.g. "fence:", "jobs:").
#
#   tools/harness/scenario_cvar_ab.sh <windows|odin2> <scenario.toml> <cvar> <value_a> <value_b> \
#       [runs=3] [prefix] [-- extra game args]
#
# The first run builds (Windows) or deploys (Odin 2); the rest reuse it. On
# the Odin 2 each run is fetched before the next. Run from the repo root in
# WSL; uses tools/harness/rs.sh. Medians: compare the printed rows.
set -u
target=$1 scen=$2 cvar=$3 va=$4 vb=$5
shift 5
runs=3 prefix=""
if [ $# -gt 0 ] && [ "$1" != "--" ]; then runs=$1; shift; fi
if [ $# -gt 0 ] && [ "$1" != "--" ]; then prefix=$1; shift; fi
[ $# -gt 0 ] && [ "$1" = "--" ] && shift
extra=("$@")
RS=${SKATE3_RS:-$(dirname "$0")/rs.sh}
name=$(basename "$scen" .toml)
order=()
for ((i = 0; i < runs; ++i)); do
  if ((i % 2 == 0)); then order+=("$va" "$vb"); else order+=("$vb" "$va"); fi
done
first=1
n=0
total=${#order[@]}
for val in "${order[@]}"; do
  n=$((n + 1))
  echo "--- run $n/$total: $cvar=$val ($(date +%T))"
  out=$(mktemp)
  if [ "$target" = windows ]; then
    opt=(--backend vulkan); [ $first = 1 ] || opt+=(--no-build)
    bash "$RS" windows "$scen" "${opt[@]}" -- "--$cvar=$val" "${extra[@]}" < /dev/null > "$out" 2>&1
    if grep -q "build.sh failed" "$out"; then
      echo "BUILD FAILED (out/harness/windows-release-build.log)"; exit 1
    fi
  else
    opt=(); [ $first = 1 ] || opt+=(--no-deploy)
    bash "$RS" odin2 "$scen" "${opt[@]}" -- "--$cvar=$val" "${extra[@]}" < /dev/null > "$out" 2>&1
    bash "$RS" fetch < /dev/null > /dev/null 2>&1
  fi
  first=0
  d=$(ls -dt out/harness-runs/*_"$name" 2>/dev/null | head -1)
  echo "  $d"
  grep -E ": (pass|fail|crash|timeout)|frame time" "$out" | head -2 | sed 's/^ */  /'
  rm -f "$out"
  [ -n "$d" ] || continue
  if [ -f "$d/profile.txt" ]; then
    for t in main render job; do
      python3 tools/harness/profile_report.py "$d" --thread $t --top 1 2>/dev/null |
        grep -m1 "ms/frame busy" | sed "s/^ */  $t: /"
    done
  fi
  if [ -n "$prefix" ]; then
    grep -h "$prefix" "$d/log.txt" | sed "s/.*$prefix//" | awk -v p="$prefix" '
      {for (i = 1; i <= NF; i++) {k = index($i, "="); if (k > 1) {v = substr($i, k + 1);
        if (v ~ /^[0-9.]+$/) s[substr($i, 1, k - 1)] += v}}}
      END {printf "  %s", p; for (k in s) printf " %s=%g", k, s[k]; print ""}'
  fi
done

#!/bin/bash
# A/B a cvar in a running live session: alternate it every PERIOD seconds for
# DURATION seconds, logging a `mark perf <tag>_<value>` at each switch (so
# perf_report.py segments the log by setting) and sampling the game's fps,
# FE top and presence every second to stdout.
#
#   tools/harness/live_cvar_ab.sh <cvar> <value_a> <value_b> [period_s=15] [duration_s=240] [tag=ab]
#
# The cvar is left at value_a at the end. Run from the repo root inside WSL;
# uses .claude/skills/harness/rs.sh for `send` (the adb setup lives there).
set -u
cvar=$1 va=$2 vb=$3 period=${4:-15} duration=${5:-240} tag=${6:-ab}
RS=${SKATE3_RS:-$(dirname "$0")/../../.claude/skills/harness/rs.sh}
send() { timeout 30 bash "$RS" send "$@" 2>/dev/null; }
state() { send query what=state | grep -o '"fps":[0-9.]*,"presence":[0-9]*,"fe_top":[0-9-]*'; }
start=$(date +%s)
val=$va
next_switch=$start
while [ $(( $(date +%s) - start )) -lt "$duration" ]; do
  now=$(date +%s)
  if [ "$now" -ge "$next_switch" ]; then
    send cvar "set.$cvar=$val" >/dev/null
    send mark "text=perf ${tag}_${val}" >/dev/null
    echo "$(date +%T) $cvar = $val"
    cur=$val
    [ "$val" = "$va" ] && val=$vb || val=$va
    next_switch=$(( now + period ))
  fi
  echo "$(date +%T) [$cvar=$cur] $(state)"
  sleep 1
done
send cvar "set.$cvar=$va" >/dev/null
send mark "text=perf end" >/dev/null
echo "$(date +%T) done; $cvar = $va"

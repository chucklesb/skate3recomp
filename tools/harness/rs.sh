#!/usr/bin/env bash
# Runs tools/harness/run_scenario.py with this machine's settings.
#
#   tools/harness/rs.sh odin2 scenarios/boot_skip.toml
#   tools/harness/rs.sh windows scenarios/boot_skip.toml --no-build
#   tools/harness/rs.sh send query what=state
#
# Per-machine settings (adb, DroidDeck, Windows stage paths, RenderDoc) live
# in .claude/local.env (gitignored; copy .claude/local.env.example). Hosts
# and setup: docs/dev-setup.md. Run it from any directory.
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -f "$repo/.claude/local.env" ]; then
  set -a
  . "$repo/.claude/local.env"
  set +a
fi
# nix (a non-login shell may not have it on PATH yet).
[ -f "$HOME/.nix-profile/etc/profile.d/nix.sh" ] && . "$HOME/.nix-profile/etc/profile.d/nix.sh"
cd "$repo" || exit 1
exec python3 -I tools/harness/run_scenario.py "$@"

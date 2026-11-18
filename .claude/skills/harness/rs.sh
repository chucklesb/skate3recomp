#!/usr/bin/env bash
# Moved to tools/harness/rs.sh (settings in .claude/local.env); kept as a forwarder.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/tools/harness/rs.sh" "$@"

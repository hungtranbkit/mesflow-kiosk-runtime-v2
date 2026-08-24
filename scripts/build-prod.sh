#!/usr/bin/env bash
# Builds the PRODUCTION profile artifact. Compiles the entire debug_server
# module out (MESFLOW_DEBUG_API=0, derived from MESFLOW_PROFILE_PROD) --
# does NOT just disable routes at runtime. See docs/SECURITY.md.
#
# This does not flash anything, and flashing this to the current dev board
# is NOT expected/required for Phase 1 -- build + host-side verification
# (e.g. `strings` on the .elf showing no /debug/* routes) is the evidence
# bar for PROD, per the task's own §70.
set -euo pipefail
exec "$(dirname "$0")/build.sh" prod

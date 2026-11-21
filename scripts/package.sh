#!/usr/bin/env bash
# Device packaging script, identical in every device repository. Do not edit it: a board changes board.toml
# ([package] and the sections it names), never this file. Steps (run by "unixnu release"): deps, preflight,
# patches, build, pack, verify. Documentation: docs/porting/packaging.md in the UniXNU repository.
# usage: scripts/package.sh [--os 17|26|27|PROFILE] [--no-deps] [--check] [any other unixnu option]
set -euo pipefail
DEVICE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UNIXNU=$(python3 - "$DEVICE/board.toml" <<'PY'
import sys, tomllib
package = tomllib.load(open(sys.argv[1], 'rb')).get('package', {})
if 'unixnu' not in package:
    sys.exit('board.toml needs [package] unixnu = "<path of the UniXNU checkout, relative to this repository>"')
print(package['unixnu'])
PY
)
case "$UNIXNU" in /*) ROOT=$UNIXNU ;; *) ROOT=$(cd "$DEVICE/$UNIXNU" && pwd) ;; esac
# iPadOS 27 sleep/wake fix (sheng). The GOP scanout has no panel power switch, so an
# display-off request left requested=0 with powered=1: the user client's state wait never
# completed, the compositor's display-mode transition stayed pending, and every later
# power-button press re-issued "off" instead of waking the panel. The patch mirrors the
# requested word into powered and logs each request as [legacy-pm]; it must run before any
# build, while the pinned UniXNU checkout still holds the pristine display source.
python3 "$DEVICE/scripts/apply-state-request.py"
exec python3 "$ROOT/unixnu" release -D "$DEVICE" "$@"

#!/usr/bin/env bash
# Like drive.sh but passes extra env from the caller; retries on the startup "not free" race (T135).
# Runs ${XCDE_EXE:-build/xcde_dev.exe} (a test copy: the user's build/xcde.exe is never killed or relinked under a
# running game) and stops by itself after the last capture. Several drives can run at once (distinct names).
#   DRIVE_EXE=<exe>  run another exe (default build/xcde_dev.exe, copied from build/xcde.exe when older or missing)
#   DRIVE_SAVE=<dir> start from a copy of that save folder (holding save_0/...), e.g. saves/colony9_20261019_1235;
#                    without it the run gets an empty save folder. The user's save/ is never used by a drive.
cd "$(dirname "$0")/.."
. scripts/env.sh
ROUTE=$1; SHOTS=$2; SECS=${3:-90}; NAME=${4:-drive}
SRC_EXE="build/xc3${XC3_EXE_EXT}"
[ -f "$SRC_EXE" ] || SRC_EXE="build/xcde${XC3_EXE_EXT}"
EXE=${DRIVE_EXE:-build/xc3_dev${XC3_EXE_EXT}}
if [ -z "$DRIVE_EXE" ] && { [ ! -f "$EXE" ] || [ "$SRC_EXE" -nt "$EXE" ]; }; then
  cp -f "$SRC_EXE" "$EXE" 2>/dev/null
  [ -f "${SRC_EXE%.exe}.pdb" ] && cp -f "${SRC_EXE%.exe}.pdb" "${EXE%.exe}.pdb" 2>/dev/null || true
fi
XS=""
for f in ${SHOTS//,/ }; do XS="$XS,$f:build/${NAME}_$f.png"; done
rm -f build/${NAME}_*.png
SD=build/save_$NAME; rm -rf "$SD"; mkdir -p "$SD"
[ -n "$DRIVE_SAVE" ] && cp -r "$DRIVE_SAVE"/. "$SD"/
export LARECOMP_SAVE_DIR=$SD
for t in 1 2 3; do
  XC3_INPUT_REPLAY=$ROUTE XC3_DETERMINISTIC=1 XCDE_INPUT_REPLAY=$ROUTE XCDE_DETERMINISTIC=1 LARECOMP_HIDDEN=1 LARECOMP_NO_AUDIO=1 \
    XC3_EXIT_AFTER_SHOTS=${XC3_EXIT_AFTER_SHOTS:-${XCDE_EXIT_AFTER_SHOTS:-1}} XCDE_EXIT_AFTER_SHOTS=${XCDE_EXIT_AFTER_SHOTS:-1} \
    XC3_SHOTS="${XS#,}" XCDE_SHOTS="${XS#,}" timeout "$SECS" "$EXE" targets > build/$NAME.log 2>&1
  rc=$?
  grep -aq "not free" build/$NAME.log || break
done
echo "rc=$rc"

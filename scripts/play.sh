#!/usr/bin/env bash
# Play the game in a window while the session is recorded for debugging:
#   scripts/play.sh [seconds-between-captures=5]
# Creates play/<timestamp>/ with shot_NNNNNN.png (window captures), input.txt (your inputs, replayable with
# XCDE_INPUT_REPLAY), game.log, and crash/ reports if it crashes. Nothing here is committed (play/ and crash/ are ignored).
cd "$(dirname "$0")/.."
. scripts/env.sh
EXE="build/xc3${XC3_EXE_EXT}"
[ -f "$EXE" ] || EXE="build/xcde${XC3_EXE_EXT}"
if [ ! -f cache/precompile_done.txt ]; then  # [XC3] shader precompilation to SPIR-V, once (~3 min; delete the file to redo)
  echo "Precompiling shaders (once, ~3 min)..."
  "$EXE" --precompile targets 2>&1 | grep -a --line-buffered "^\[precompile\] [0-9]* files\|FATAL"
fi
EVERY=${1:-5}
D=play/$(date +%Y%m%d_%H%M%S)
mkdir -p "$D"
echo "Recording to $D (captures every ${EVERY}s). Close the window to stop."
XC3_SHOT_EVERY=$((EVERY * 30)):$D XC3_INPUT_RECORD=$D/input.txt XC3_STALL=5 XCDE_SHOT_EVERY=$((EVERY * 30)):$D XCDE_INPUT_RECORD=$D/input.txt XCDE_STALL=5 "$EXE" targets 2>&1 | tee "$D/game.log" | grep -a --line-buffered "STRICT\|\[crash\]\|\[stall\]\|FATAL" 
echo "Done. Send me the folder $D (or tell me what you saw)."

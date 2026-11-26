#!/usr/bin/env bash
# Build the recompiled game code objects (build/obj/) with the project venv's zig cc.
#   1. python tools/gen_all.py            -> generated/{main,sdk}
#   2. this script                        -> build/obj/*.o
# JOBS=N sets the number of parallel compiles (default: by CPU count and memory, see below).
set -e
cd "$(dirname "$0")/.."
. scripts/env.sh
Z="$XC3_CC"
# [XC3] Run the zig binary itself in place of `python -m ziglang cc`: the module only executes <package>/zig, and
# starting a Python for every one of thousands of files adds up. A relative path, as the repo path may hold spaces.
# Falls back to XC3_CC when the package is not found (or XC3_CC was set to something else).
case "$Z" in
  *" -m ziglang cc")
    zd=$("$XC3_PY" -c "import ziglang, os; print(os.path.relpath(os.path.dirname(ziglang.__file__)).replace(os.sep, '/'))" 2>/dev/null || true)
    if [ -n "$zd" ]; then
      for zx in "$zd/zig" "$zd/zig.exe"; do [ -x "$zx" ] && { Z="$zx cc"; break; }; done
    fi
    ;;
esac
export Z
CF="-O1 $XC3_ARCH -ffp-contract=off -fno-strict-aliasing -fvisibility=hidden -Iruntime -w"; export CF
# [XC3] 64-bit off_t / stat for the runtime objects too (build_rt_gfx.sh)
CF="$CF -D_FILE_OFFSET_BITS=64"
# [XC3] Parallel compiles by CPU count and memory, as xc3-recomp's tools/build_game.py does: one compile at -O1 peaks
# at 120-180 MiB for a ~3 MiB chunk and more for the function tables, so 768 MiB per job within 80% of RAM keeps a
# smaller machine from swapping. /proc/meminfo exists on Linux and in MSYS2.
if [ -z "${JOBS:-}" ]; then
  JOBS=$(nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-4}")
  mem_kib=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || true)
  if [ -n "$mem_kib" ]; then
    by_mem=$(( mem_kib * 8 / 10 / (768 * 1024) ))
    [ "$by_mem" -lt "$JOBS" ] && JOBS=$by_mem
  fi
  [ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=1
fi
mkdir -p build/obj/main build/obj/sdk build/rt
# drop objects whose chunk no longer exists (the chunk layout shifts when the generator output changes)
for o in build/obj/*/*.o; do m=$(basename "$(dirname "$o")"); b=$(basename "$o" .o); [ -f "generated/$m/$b.c" ] || rm -f "$o" "$o.d"; done
# [XC3] Other compile flags or compiler than the objects were built with (a new CPU level, XC3_MARCH): every object is
# stale. Dropping the dependency files makes the pass below take them all, and a build stopped halfway still finishes
# them on the next run.
flags="$Z $CF -MMD"
if [ "$(cat build/obj/.cflags 2>/dev/null)" != "$flags" ]; then
  rm -f build/obj/*/*.o.d
  printf '%s\n' "$flags" > build/obj/.cflags
fi
# [XC3] What to compile, in one Python pass: a chunk whose object or dependency file (-MMD, written next to the object)
# is missing, or whose source or any header it includes from runtime/ is newer than the object. Objects from before
# the dependency files existed are therefore compiled once more.
todo=build/obj/.todo
"$XC3_PY" -X utf8 - "$todo" <<'PY'
import glob, os, re, sys
mtimes = {}
def mtime(p):
    if p not in mtimes:
        try:
            mtimes[p] = os.path.getmtime(p)
        except OSError:
            mtimes[p] = None
    return mtimes[p]
todo = []
for c in sorted(glob.glob('generated/main/*.c') + glob.glob('generated/sdk/*.c')):
    c = c.replace(os.sep, '/')
    o = 'build/obj/%s/%s.o' % (os.path.basename(os.path.dirname(c)), os.path.basename(c)[:-2])
    t_o = mtime(o)
    deps = []
    if t_o is not None and os.path.isfile(o + '.d'):
        text = open(o + '.d', encoding='utf-8', errors='replace').read().replace('\\\n', ' ')
        if ':' in text:  # "<object>: <source> <headers...>", spaces in a path escaped as "\ "
            deps = [d.replace('\\ ', ' ') for d in re.findall(r'(?:\\ |\S)+', text.split(':', 1)[1])]
    if not deps or not all(mtime(d) is not None and mtime(d) < t_o for d in deps):
        todo.append(c)
with open(sys.argv[1], 'w', newline='\n') as f:
    f.write(''.join(c + '\n' for c in todo))
PY
TOTAL=$(( $(wc -l < "$todo") )); export TOTAL
echo "game code: $TOTAL of $(ls generated/main/*.c generated/sdk/*.c | wc -l) files to compile, $JOBS jobs, CPU level ${XC3_CPU_LEVEL:-v2} ($Z)"
# [XC3] Progress: cc1 prints "DONE <chunk>" and one reader numbers them, so the count is exact with parallel jobs.
# The object is touched after a compile: zig cc copies an object from its cache with that object's old mtime, which
# would leave it older than a touched source or header and compiled again on every run.
cc1() {
  f=$1; m=$(basename "$(dirname "$f")"); b=$(basename "$f" .c); o="build/obj/$m/$b.o"
  if $Z -c "$f" -o "$o" $CF -MMD -MF "$o.d"; then touch "$o"; else rm -f "$o" "$o.d"; echo "FAIL $f"; fi
  echo "DONE $m/$b"
}
export -f cc1
n=0
xargs -P "$JOBS" -I{} bash -c 'cc1 "$1"' _ {} < "$todo" | while IFS= read -r line; do
  case "$line" in
    "DONE "*) n=$((n + 1)); echo "[$n/$TOTAL] ${line#DONE }" ;;
    *) echo "$line" ;;
  esac
done

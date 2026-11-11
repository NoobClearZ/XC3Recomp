"""XC3 map jump tour in batches: hidden game runs that fast-travel through a list of jump points, one process per chunk.

    .venv/Scripts/python tools/xc3_tour.py [--maps 18,19] [--every 4] [--limit 40] [--chunk 40] [--shots]

Each chunk: a scratch copy of the save (LARECOMP_SAVE_DIR - the real save is never touched), the input replay that
loads it (title screen -> Continue), then XCDE_TOUR (runtime/hle/party.c xc3_tour) from --start: one `jump <map>
<hash>` per point, --wait frames each. Out: <out>/chunkNN/{tour.txt,game.log,shots/} and <out>/summary.tsv, one row
per step: reached or not, the worst frame time and the build waits ([vk] lines) logged until the next step.

The jump list (build/tours/xc3_jumps.txt) is made from the game files (SYS_MapJumpList) - never commit it.
"""
import argparse, glob, os, re, shutil, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def save_for(replay):
    """The save backup the recorded session started from: play/<stamp>/ -> the first save/backup/save_0_<stamp'> at or
    after it (fs.c backs the save up at each launch, a second or two after play.sh names the folder)."""
    stamp = os.path.basename(os.path.dirname(os.path.abspath(replay)))
    for p in sorted(glob.glob(os.path.join(REPO, 'save', 'backup', 'save_0_*'))):
        if os.path.basename(p)[len('save_0_'):] >= stamp:
            return p
    return None


def read_jumps(path, maps, every, limit, skip):
    out = []
    for line in open(path, encoding='utf-8'):
        f = line.split()
        if len(f) == 3 and f[0] == 'jump' and (not maps or int(f[1]) in maps):
            out.append((int(f[1]), f[2]))
    out = out[skip::every]
    return out[:limit] if limit else out


VK = re.compile(r'\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms.*?build waits (\d+) \(([\d.]+)ms\)')
STEP = re.compile(r'\[tour\] frame (\d+): step (\d+) (jump\S*) (\d+) (\S+)')


def summarize(log_path, steps):
    """Per tour step: frame it started, worst frame ms / lowest fps / build waits of the [vk] lines after it."""
    rows = {i: dict(frame='', max_ms=0, min_fps='', waits=0, wait_ms=0.0) for i in range(len(steps))}
    cur, done, retries = None, False, 0
    for line in open(log_path, encoding='utf-8', errors='replace'):
        m = STEP.search(line)
        if m:
            cur = int(m.group(2))
            if cur in rows: rows[cur]['frame'] = int(m.group(1))
            continue
        if '[tour] done' in line: done = True; cur = None; continue
        if 'no map change manager' in line: retries += 1
        m = VK.search(line)
        if m and cur in rows:
            r = rows[cur]
            r['max_ms'] = max(r['max_ms'], int(m.group(4)))
            fps = float(m.group(3))
            r['min_fps'] = fps if r['min_fps'] == '' else min(r['min_fps'], fps)
            r['waits'] += int(m.group(5))
            r['wait_ms'] += float(m.group(6))
    return rows, done, retries


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--list', default=os.path.join(REPO, 'build', 'tours', 'xc3_jumps.txt'))
    ap.add_argument('--maps', default='', help='map ids to keep, e.g. 18,19 (default: all)')
    ap.add_argument('--every', type=int, default=1, help='every Nth point')
    ap.add_argument('--skip', type=int, default=0, help='start at the Nth point (after --maps)')
    ap.add_argument('--limit', type=int, default=0, help='at most N points')
    ap.add_argument('--chunk', type=int, default=40, help='points per game process')
    ap.add_argument('--wait', type=int, default=600, help='frames after each jump (XCDE_TOUR_WARP_WAIT)')
    ap.add_argument('--start', type=int, default=3200, help='first step frame (after the replay has loaded the save)')
    ap.add_argument('--replay', default=os.path.join(REPO, 'play', '20261106_001937', 'input.txt'),
                    help='input replay that loads the save (title screen -> Continue)')
    ap.add_argument('--save', default=None, help='folder with the save files (a save_0 copy); default: the backup made when --replay was recorded')
    ap.add_argument('--exe', default=os.path.join(REPO, 'build', 'xc3.exe'))
    ap.add_argument('--out', default=None, help='default: build/tour_runs/<date_time>')
    ap.add_argument('--shots', action='store_true', help='a capture after each jump (XCDE_TOUR_SHOT_AUTO)')
    ap.add_argument('--frame-times', action='store_true', help='every frame time into chunkNN/frames.txt (XC3_FRAME_TIMES; tools/perf_compare.py)')
    ap.add_argument('--env', action='append', default=[], help='extra NAME=VALUE for the game (repeatable)')
    ap.add_argument('--timeout', type=float, default=0, help='seconds per run (default: from the steps; more after a shader change)')
    a = ap.parse_args()

    save = a.save or save_for(a.replay)
    for what, p in (('jump list', a.list), ('replay', a.replay), ('save', save), ('exe', a.exe)):
        if not p or not os.path.exists(p):
            sys.exit(f'missing {what}: {p}')
    maps = {int(x) for x in a.maps.split(',') if x.strip()}
    jumps = read_jumps(a.list, maps, max(1, a.every), a.limit, a.skip)
    if not jumps:
        sys.exit('no jump points selected')
    out = os.path.abspath(a.out or os.path.join(REPO, 'build', 'tour_runs', time.strftime('%Y%m%d_%H%M%S')))
    os.makedirs(out, exist_ok=True)
    chunks = [jumps[i:i + a.chunk] for i in range(0, len(jumps), a.chunk)]
    print(f'tour: {len(jumps)} points in {len(chunks)} runs, save {save}, out {out}', flush=True)

    summary = open(os.path.join(out, 'summary.tsv'), 'w', encoding='utf-8')
    summary.write('chunk\tstep\tmap\thash\treached_frame\tmax_frame_ms\tmin_fps\tbuild_waits\tbuild_wait_ms\n')
    totals = dict(reached=0, missed=0, crashed=0, timeouts=0)
    for ci, steps in enumerate(chunks):
        work = os.path.join(out, f'chunk{ci:02d}')
        os.makedirs(work, exist_ok=True)
        tour = os.path.join(work, 'tour.txt')
        with open(tour, 'w', encoding='utf-8') as f:
            for m, h in steps:
                f.write(f'jump {m} {h}\n')
        saves = os.path.join(work, 'save')
        shutil.rmtree(saves, ignore_errors=True)
        shutil.copytree(save, os.path.join(saves, 'save_0'))  # (LARECOMP_SAVE_DIR = the folder HOLDING save_0)
        env = dict(os.environ, XCDE_TOUR=tour, XCDE_TOUR_EXIT='1', XCDE_TOUR_START=str(a.start),
                   XCDE_TOUR_WARP_WAIT=str(a.wait), XCDE_INPUT_REPLAY=os.path.abspath(a.replay),
                   LARECOMP_SAVE_DIR=saves, LARECOMP_HIDDEN='1', LARECOMP_AUDIO_GAIN='0')
        if a.shots:
            env.update(XCDE_TOUR_SHOT_AUTO='1', XCDE_TOUR_SHOTS=os.path.join(work, 'shots'))
        if a.frame_times:
            env['XC3_FRAME_TIMES'] = os.path.join(work, 'frames.txt')
        for kv in a.env:
            k, _, v = kv.partition('=')
            env[k] = v
        env['PATH'] = 'C:\\msys64\\ucrt64\\bin;' + env.get('PATH', '')
        # (wall clock: the replay ~70 s, then each jump's frames at >= ~20 fps plus a cold load)
        limit = a.timeout or 180 + len(steps) * (a.wait / 20 + 20)
        log_path = os.path.join(work, 'game.log')
        t0 = time.time()
        with open(log_path, 'w', encoding='utf-8') as log:
            p = subprocess.Popen([a.exe, 'targets'], cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                code = p.wait(timeout=limit)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
                code = 'timeout'
        rows, done, retries = summarize(log_path, steps)
        reached = sum(1 for r in rows.values() if r['frame'] != '')
        state = 'timeout' if code == 'timeout' else 'done' if done else f'exit {code}'
        totals['reached'] += reached
        totals['missed'] += len(steps) - reached
        totals['crashed'] += state.startswith('exit')
        totals['timeouts'] += state == 'timeout'
        for i, (m, h) in enumerate(steps):
            r = rows[i]
            summary.write(f"{ci}\t{i}\t{m}\t{h}\t{r['frame']}\t{r['max_ms']}\t{r['min_fps']}\t{r['waits']}\t{r['wait_ms']:.0f}\n")
        summary.flush()
        worst = max(rows.values(), key=lambda r: r['max_ms'])
        print(f'chunk {ci:02d}: {state}, {reached}/{len(steps)} steps, {time.time() - t0:.0f} s, worst frame {worst["max_ms"]} ms'
              + (f', manager retries {retries}' if retries else ''), flush=True)
    summary.close()
    print(f"tour: reached {totals['reached']}, not reached {totals['missed']}, crashed runs {totals['crashed']}, "
          f"timeouts {totals['timeouts']} -> {os.path.join(out, 'summary.tsv')}", flush=True)
    return 1 if totals['crashed'] or totals['timeouts'] else 0


if __name__ == '__main__':
    sys.exit(main())

"""Compare game runs side by side: frame rate, 1% low, slow frames, build waits.

    .venv/Scripts/python tools/perf_compare.py <run> [<run> ...] [--from-frame N] [--to-frame N]

A run is a game.log, or a folder holding game.log files (a play/<stamp> session, a tools/xc3_tour.py output with
chunkNN/ folders: its chunks are added up). Beside each game.log a frames.txt is used when there is one: the game's
XC3_FRAME_TIMES=<file> output, "<frame> <microseconds since the last present>" per line (xc3_tour.py --frame-times
writes it). Without it the frame figures come from the 2-second "[vk]" lines and are marked "~": an average and the
worst frame only, no 1% low and no counts.

Only frames from the first "[tour] ... step" line to "[tour] done" count when the log has them (the boot, the title
screen and the save loading are left out); --from-frame / --to-frame set the range by hand.
"""
import argparse, glob, os, re, sys

VK = re.compile(r'\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms')
STEP = re.compile(r'\[tour\] frame (\d+): step \d+ ')
DONE = re.compile(r'\[tour\] done at frame (\d+)')
WAIT = re.compile(r'\[build-wait\] (program|pipeline) ([\d.]+) ms')
HOLD = re.compile(r'\[prewarm\] .* held (\d+) ms')
HITCH = re.compile(r'\[hitch\] frame (\d+): ([\d.]+) ms \|')


def logs_of(path):
    if os.path.isfile(path):
        return [path]
    found = sorted(glob.glob(os.path.join(path, '**', 'game.log'), recursive=True))
    if not found:
        sys.exit(f'no game.log in {path}')
    return found


def frame_range(log, lo, hi):
    """(first, last) frame to count: the tour's steps when the log has them, else everything."""
    first = last = None
    for line in open(log, encoding='utf-8', errors='replace'):
        m = STEP.search(line)
        if m and first is None:
            first = int(m.group(1))
        m = DONE.search(line)
        if m:
            last = int(m.group(1))
    return (lo if lo is not None else first or 0), (hi if hi is not None else last or 1 << 62)


def read_log(log, lo, hi):
    """What the log has for frames lo..hi: [vk] windows, build waits, loading holds, [hitch] lines."""
    r = dict(vk=[], prog=[], pipe=[], hold=[], hitch=0)
    frame, prev = 0, None  # (lines without a frame number belong to the last [vk] / [tour] frame seen)
    for line in open(log, encoding='utf-8', errors='replace'):
        m = VK.search(line)
        if m:
            t, f = float(m.group(1)), int(m.group(2))
            if prev and lo <= prev[1] < f <= hi:  # (windows wholly inside the range)
                r['vk'].append((t - prev[0], f - prev[1], int(m.group(4))))
            prev, frame = (t, f), f
            continue
        m = STEP.search(line) or DONE.search(line)
        if m:
            frame = int(m.group(1))
            continue
        if not lo <= frame <= hi:
            continue
        m = WAIT.search(line)
        if m:
            r['prog' if m.group(1) == 'program' else 'pipe'].append(float(m.group(2)))
            continue
        m = HOLD.search(line)
        if m:
            r['hold'].append(float(m.group(1)))
        elif HITCH.search(line):
            r['hitch'] += 1
    return r


def read_frames(path, lo, hi):
    """Frame times in ms of frames lo..hi from an XC3_FRAME_TIMES file."""
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        f = line.split()
        if len(f) == 2 and f[0].isdigit() and f[1].isdigit() and lo <= int(f[0]) <= hi:
            out.append(int(f[1]) / 1000.0)
    return out


def frame_stats(ms):
    """avg fps, 1% low (the fps of the slowest 1% of frames), frames over 25 / 100 ms, worst frame."""
    if not ms:
        return None
    slow = sorted(ms, reverse=True)[:max(1, len(ms) // 100)]
    return dict(frames=len(ms), seconds=sum(ms) / 1000.0, fps=1000.0 * len(ms) / sum(ms),
                low1=1000.0 * len(slow) / sum(slow), over25=sum(1 for x in ms if x > 25.0),
                over100=sum(1 for x in ms if x > 100.0), worst=max(ms))


def measure(path, lo=None, hi=None):
    ms, exact, tot = [], True, dict(vk=[], prog=[], pipe=[], hold=[], hitch=0)
    for log in logs_of(path):
        a, b = frame_range(log, lo, hi)
        r = read_log(log, a, b)
        for k in ('vk', 'prog', 'pipe', 'hold'):
            tot[k] += r[k]
        tot['hitch'] += r['hitch']
        ft = os.path.join(os.path.dirname(log), 'frames.txt')
        if os.path.isfile(ft):
            ms += read_frames(ft, a, b)
        else:
            exact = False
    s = frame_stats(ms) if exact else None
    if s is None and tot['vk']:  # the [vk] lines: an average and the worst frame only
        sec, n = sum(v[0] for v in tot['vk']), sum(v[1] for v in tot['vk'])
        s = dict(frames=n, seconds=sec, fps=n / sec if sec else 0.0, low1=None, over25=None, over100=None,
                 worst=float(max(v[2] for v in tot['vk'])))
    s = s or dict(frames=0, seconds=0.0, fps=0.0, low1=None, over25=None, over100=None, worst=0.0)
    s.update(exact=exact and bool(ms), prog_n=len(tot['prog']), prog_ms=sum(tot['prog']), pipe_n=len(tot['pipe']),
             pipe_ms=sum(tot['pipe']), hold_n=len(tot['hold']), hold_ms=sum(tot['hold']), hitch=tot['hitch'])
    return s


ROWS = [('frames', 'frames', '{:.0f}'), ('seconds', 'seconds', '{:.1f}'), ('fps', 'average fps', '{:.1f}'),
        ('low1', '1% low fps', '{:.1f}'), ('over25', 'frames over 25 ms', '{:.0f}'),
        ('over100', 'frames over 100 ms', '{:.0f}'), ('worst', 'worst frame ms', '{:.0f}'),
        ('prog_n', 'program waits', '{:.0f}'), ('prog_ms', 'program wait ms', '{:.0f}'),
        ('pipe_n', 'pipeline waits', '{:.0f}'), ('pipe_ms', 'pipeline wait ms', '{:.0f}'),
        ('hold_n', 'loading holds', '{:.0f}'), ('hold_ms', 'loading hold ms', '{:.0f}'), ('hitch', '[hitch] lines', '{:.0f}')]
FRAME_KEYS = ('frames', 'seconds', 'fps', 'low1', 'over25', 'over100', 'worst')


def table(names, stats):
    lines = ['{:<20}'.format('') + ''.join('{:>16}'.format(n[-15:]) for n in names) + ('{:>12}'.format('change') if len(stats) == 2 else '')]
    for key, label, fmt in ROWS:
        cells = []
        for s in stats:
            v = s[key]
            cells.append('n/a' if v is None else ('~' if key in FRAME_KEYS and not s['exact'] else '') + fmt.format(v))
        line = '{:<20}'.format(label) + ''.join('{:>16}'.format(c) for c in cells)
        if len(stats) == 2:
            a, b = stats[0][key], stats[1][key]
            line += '{:>12}'.format('' if a is None or b is None or not a else '{:+.1f}%'.format(100.0 * (b - a) / a))
        lines.append(line)
    return '\n'.join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--from-frame', type=int, default=None)
    ap.add_argument('--to-frame', type=int, default=None)
    a = ap.parse_args(argv)
    stats = [measure(p, a.from_frame, a.to_frame) for p in a.runs]
    print(table([os.path.basename(os.path.normpath(p)) for p in a.runs], stats))
    if not all(s['exact'] for s in stats):
        print('~: from the 2-second [vk] lines (no frames.txt: run with XC3_FRAME_TIMES / xc3_tour.py --frame-times)')
    return 0


if __name__ == '__main__':
    sys.exit(main())

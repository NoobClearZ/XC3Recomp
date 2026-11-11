import os, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import perf_compare as pc  # noqa: E402

LOG = '''[vk] t=10.0 frame 100 30.0 fps max 34ms level  (1 draws, build waits 0 (0.0ms))
[build-wait] pipeline 50.0 ms: queue 0.0 build 50.0 linked (before the tour: not counted)
[tour] frame 200: step 0 jump 18 aaaa
[vk] t=12.0 frame 300 50.0 fps max 40ms level  (1 draws)
[build-wait] program 12.5 ms: queue 0.0 lock 0.0
[build-wait] pipeline 30.0 ms: queue 0.0 build 30.0 linked
[prewarm] scene start (3000 draws, 20 programs registered) held 200 ms for 5 builds
[hitch] frame 350: 120.0 ms | draws waited 100.0 ms (3)
[vk] t=14.0 frame 400 50.0 fps max 120ms level  (1 draws)
[tour] done at frame 400
[vk] t=16.0 frame 500 50.0 fps max 900ms level  (1 draws)
[build-wait] pipeline 70.0 ms: queue 0.0 (after the tour: not counted)
'''


def write_run(d, frames=None):
    with open(os.path.join(d, 'game.log'), 'w', encoding='utf-8') as f:
        f.write(LOG)
    if frames is not None:
        with open(os.path.join(d, 'frames.txt'), 'w', encoding='utf-8') as f:
            for n, us in frames:
                f.write(f'{n} {us}\n')


class PerfCompareTest(unittest.TestCase):
    def test_frame_stats(self):
        s = pc.frame_stats([10.0] * 198 + [30.0, 160.0])
        self.assertEqual(s['frames'], 200)
        self.assertEqual(s['over25'], 2)
        self.assertEqual(s['over100'], 1)
        self.assertEqual(s['worst'], 160.0)
        self.assertAlmostEqual(s['fps'], 200 * 1000.0 / 2170.0)
        self.assertAlmostEqual(s['low1'], 2 * 1000.0 / 190.0)  # (the slowest 1% = 2 frames)

    def test_frames_file_and_tour_range(self):
        with tempfile.TemporaryDirectory() as d:
            # frames 150..450, 20 ms each; one 120 ms frame inside the tour, one 900 ms frame after it
            write_run(d, [(n, 120000 if n == 350 else 900000 if n == 450 else 20000) for n in range(150, 451)])
            s = pc.measure(d)
        self.assertTrue(s['exact'])
        self.assertEqual(s['frames'], 201)  # (200..400)
        self.assertEqual(s['over100'], 1)
        self.assertEqual(s['worst'], 120.0)
        self.assertEqual((s['prog_n'], s['prog_ms']), (1, 12.5))
        self.assertEqual((s['pipe_n'], s['pipe_ms']), (1, 30.0))
        self.assertEqual((s['hold_n'], s['hold_ms']), (1, 200.0))
        self.assertEqual(s['hitch'], 1)

    def test_vk_lines_without_frames_file(self):
        with tempfile.TemporaryDirectory() as d:
            write_run(d)
            s = pc.measure(d)
            out = pc.table(['a', 'b'], [s, s])
        self.assertFalse(s['exact'])
        self.assertEqual(s['frames'], 100)  # (the one window wholly inside the tour: 300..400)
        self.assertAlmostEqual(s['fps'], 50.0)
        self.assertEqual(s['worst'], 120.0)
        self.assertIsNone(s['low1'])
        self.assertIn('~50.0', out)
        self.assertIn('n/a', out)

    def test_chunks_are_added_up(self):
        with tempfile.TemporaryDirectory() as d:
            for c in ('chunk00', 'chunk01'):
                os.makedirs(os.path.join(d, c))
                write_run(os.path.join(d, c), [(n, 20000) for n in range(200, 401)])
            s = pc.measure(d)
        self.assertEqual(s['frames'], 402)
        self.assertEqual(s['pipe_n'], 2)


if __name__ == '__main__':
    unittest.main()

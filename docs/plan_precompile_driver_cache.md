# Rencana: precompile → cache driver (penyelidikan, belum perbaikan)

Branch: `perf-stutter`. Lanjutan dari `docs/plan_perf_stutter.md` Phase 1 (1.0 selesai, "loading holds" belum).
Jawab user dalam Bahasa Indonesia. Alur: selidiki → laporkan (terbukti vs belum) → tunggu persetujuan → baru perbaiki.

## Yang sudah terbukti (dari kode, 2026-11-13)

- `xc3.exe --precompile <dump>` (`runtime/host/main_boot.c:133`) memakai device Vulkan game sendiri di jendela
  tersembunyi (`precompile.c:720-722`), jadi semua yang dibangun di sana masuk cache disk driver yang sama.
- Selama precompile, `cache/vk_pipelines.bin` sengaja dimatikan (`precompile.c:729-733`, `g_vk.pcache = NULL`):
  hasilnya hanya masuk cache driver, bukan file aplikasi.
- Tiga fase precompile:
  1. Main (semua shader `bf3.ard`): **SPIR-V saja**, tanpa objek driver (`XCDE_PRECOMPILE_LIBS` mati secara default;
     catatan lama: 40 menit dan tidak ada manfaatnya).
  2. Seed (6,729 varian): **pipeline library** pre-raster + fragment (commit `e0a89ae`) → masuk cache driver.
  3. Variants (`var_worker`): pipeline utuh yang dioptimasi → masuk cache driver.
- Hasil 1.0 (tour 8 titik, exe bernama baru): pipeline waits 148 (9,9 s) → 2 (70 ms). Tapi **loading holds tetap
  ~9,4 s** (warm: ~0,3 s), dan frame >100 ms yang tersisa (32) berasal dari holds ini.
- Cache driver AMD dikunci per nama exe (plan_perf_stutter.md baris 42).

## Pertanyaan yang harus dijawab

Q1. Loading holds pada cache dingin menunggu apa? Kandidat: program di luar seed, program geometry shader
    (shadow), pipeline utuh teroptimasi, atau bukan build pipeline sama sekali (SPIR-V, I/O, hal lain).
Q2. Untuk pipeline yang memang sudah dibangun precompile: apakah game benar-benar mendapat hit di cache driver?
    Yang mungkin membuat key berbeda (belum terverifikasi): flags create-info, `pcache` NULL (precompile) vs tidak
    NULL (game), layout/render state, `RETAIN_LINK_TIME_OPTIMIZATION_INFO`.
Q3. Apakah cache driver AMD punya batas ukuran yang membuang entri? Lokasi folder cache driver AMD di Windows
    belum diverifikasi: cari dulu, ukur ukurannya sebelum/sesudah precompile dan sesudah tour. Jangan menebak.
Q4. Apakah precompile yang dipanggil launcher memakai nama exe yang sama dengan game (`xc3.exe`)? Kalau berbeda,
    cache driver tidak berguna untuk game.

## Langkah

0. Baca `docs/plan_perf_stutter.md` (Phase 0-1), `precompile.c:695-805`, bagian GPL di `exec.c` (~2525-2630,
   3023, 3257, 3461-3500, 5884). Query `graphify-out` dulu sebelum grep kode runtime.
1. Instrumentasi sementara, env-gated (mis. `XC3_HOLD_LOG=<file>`): untuk setiap loading hold catat apa yang
   ditunggu (hash program, jenis: lib pre-raster / lib fragment / pipeline utuh / GS / non-pipeline), durasi, dan
   apakah ada di seed. Hook debug sementara di `games/xc3/patches.py` + runtime; buang sebelum commit.
2. Run dingin: salin `xc3.exe` dengan nama baru → `--precompile` dengan exe itu → tour yang sama dengan baseline
   (`--maps 18,19 --every 20 --limit 8 --wait 1200 --start 4500`, opsi 60, `XC3_REALTIME=0`, `--frame-times`).
   Bandingkan dengan `tools/perf_compare.py`. Simpan run di `build/tour_runs/`.
3. Kelompokkan holds dari langkah 1 (jumlah + total ms per jenis) → jawab Q1.
4. Q2: untuk satu-dua pipeline yang dibangun precompile tapi tetap ditunggu di game, bandingkan create-info
   precompile vs game field per field (log keduanya). Opsional: run game kedua kali dengan exe yang sama untuk
   memastikan holds hilang saat cache benar-benar hangat (pembanding).
5. Q3/Q4: ukuran folder cache driver + cek bagaimana launcher memanggil precompile.
6. **Berhenti dan laporkan**: penyebab per jenis hold (terbukti/belum) dengan angka, dan usulan perbaikan dengan
   perkiraan biaya waktu precompile. Kandidat usulan: perluas seed (1.3), bangun library GS, samakan create-info,
   atau 1.4 (bangun pipeline seed saat boot). Jangan perbaiki atau commit tanpa persetujuan user.

## Aturan

- Hanya XC3; jangan ubah perilaku XCDE.
- Save: pakai salinan scratch lewat `LARECOMP_SAVE_DIR`, jangan pernah save asli.
- Setiap boot di folder `play/xc3_bootN` baru dengan junction `targets`.
- Run tersembunyi tidak menulis `cache/vk_pipelines.bin` (memang begitu, `device.c:415`); jangan pakai
  `XCDE_SAVE_CACHE=1` kecuali disengaja.
- Setelah mengubah kode: `graphify update .`, lalu smoke tour (jangan hanya cek statis).
- Selesai = laporan di atas + angka `perf_compare.py`; instrumentasi sementara sudah dibuang atau dibiarkan
  di working tree tanpa commit, disebutkan di laporan.

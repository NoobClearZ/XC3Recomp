# Rencana: hentikan prebuild library "tebakan" untuk program tanpa varian (XC3)

Branch: `perf-stutter`. Lanjutan dari `docs/plan_precompile_driver_cache.md` dan `docs/plan_perf_stutter.md` Phase 1.
Jawab user dalam Bahasa Indonesia. Alur kerja: kerjakan → ukur → laporkan → **tunggu persetujuan sebelum commit**.

## Latar belakang (sudah diukur 2026-11-13)

- Saat game mendaftarkan program, `prewarm_drain()` (`runtime/nvn/vulkan/exec.c`, blok berkomentar
  `[XCDE] T211: no variant seen yet ... the variant precompilation guesses`) membangun pipeline library untuk setiap
  program yang belum punya varian di `cache/variants.bin` atau di seed. Shader key-nya **tebakan**
  (`glsl = 1`, `depth_minus_one = true`). Job-nya masuk `pl_pending` lalu ke `gpl_prebuild`.
- `vk_prewarm_hold()` menunggu semua job itu selesai, jadi loading screen ikut tertahan.
- Hasil run dingin dengan setelan baseline (`build/tour_runs/q13_cold_probe`):
  - Dari 1.607 program tebakan, **0** library dipakai draw dengan key yang sama.
  - 49 vertex shader no_fs yang mahal (858 detik waktu worker, 17–37 detik per compile, compile murni di driver)
    **tidak pernah digambar**.
  - Program dengan fragment shader: 2.242 library, 126 detik waktu worker, tidak pernah digambar.
  - Library dari varian yang tercatat (seed): 733 dipakai dengan key yang sama, biayanya di bawah 1 detik.
- Belum terbukti: rute di luar 8 titik peta 18 (pertarungan, peta lain). Juga belum diverifikasi apakah compile
  dengan key tebakan memanaskan cache per-stage di driver AMD.

## Keputusan user

Cara 1: program tanpa varian **tidak di-prebuild sama sekali**, dengan saklar env untuk kembali ke perilaku lama.
Hanya untuk XC3; repo ini sekarang khusus XC3, jadi tidak perlu gating XCDE.

## Langkah

0. **Bersihkan working tree dulu.**
   - Instrumentasi sementara dari sesi pengukuran masih ada di `runtime/nvn/vulkan/exec.c` (17 baris bertanda
     `TEMP PROBE`, aktif lewat `XC3_GPL_PROBE`). Simpan sebagai patch untuk dipakai lagi kalau perlu, lalu kembalikan
     file itu:
     `git diff runtime/nvn/vulkan/exec.c > build/tour_runs/gpl_probe.patch`, lalu `git checkout -- runtime/nvn/vulkan/exec.c`.
   - Pastikan `git status` hanya menyisakan file rencana `docs/plan_*.md` yang belum di-commit.
   - `build/xc3.exe` sekarang masih berisi instrumentasi; build ulang di langkah 2.
   - Query `graphify-out` dulu sebelum grep kode runtime.
1. **Perubahan kode, satu tempat:** di `prewarm_drain()`, cabang `else if (prebuild_libs_on() && pl_npending < PL_MAX)`
   (blok T211 di atas).
   - Tambahkan `static bool guess_prebuild_on(void)` dengan pola yang sama seperti `prebuild_libs_on()`: baca
     `XC3_PREBUILD_GUESS` sekali; default **0 (mati)**, `=1` kembali ke perilaku lama.
   - Saat mati, lewati seluruh blok itu, termasuk `get_program()` dengan key tebakan. Terjemahan dengan key tebakan
     juga terbuang, karena SPIR-V bergantung pada key.
   - Tulis satu baris log saat pertama kali dilewati, misalnya
     `[prewarm] XC3: no guessed libraries for programs without a recorded variant (XC3_PREBUILD_GUESS=1: on)`.
   - Komentar `[XC3]` singkat yang menyebut angka pengukuran di atas (0 dari 1.607 dipakai; ~980 detik waktu worker;
     loading hold menunggunya).
   - Jangan ubah fase seed precompile, `vk_prewarm_hold`, atau jalur varian yang tercatat.
2. **Build ulang dari `perf-stutter`.** Sesi sebelumnya menemukan `build/obj/main` hasil branch `xc3-qol` sehingga
   link gagal: pastikan kode game diregenerasi dan dibangun dari branch ini. Pakai perintah build yang tercantum di
   dokumentasi repo; jangan menebak.
3. **Ukur A/B dengan cache driver dingin.** Setiap run memakai salinan `xc3.exe` dengan nama baru dan
   `--precompile` dengan exe itu. Setelan harus sama dengan baseline, karena run tanpa `XC3_INI` jalan di 30 fps dan
   tidak sebanding:
   - `XC3_INI=xc3_60.ini`, replay `load60.txt`, `--save save/keep/base_20261109_233051` (lewat salinan scratch
     `LARECOMP_SAVE_DIR`, jangan pernah save asli), `XC3_REALTIME=0`.
   - Tour: `--maps 18,19 --every 20 --limit 8 --wait 1200 --start 4500 --frame-times`.
   - Run **A**: `XC3_PREBUILD_GUESS=1` (perilaku lama). Run **B**: default.
   - Run **B-warm**: B diulang dengan exe yang sama (cache sudah hangat), untuk memastikan tidak ada regresi saat warm.
   - Bandingkan dengan `tools/perf_compare.py`, bersama `p0_cold`, `q2_cold_min` dan `p0_warm_b`. Simpan run di
     `build/tour_runs/`.
   - Catat juga total loading hold sepanjang run (boot, title, loading save), bukan hanya selama tour. Di run lama
     totalnya ~95 detik.
4. **Rute yang belum teruji:** satu run dingin B (dan A sebagai pembanding) yang memuat pertarungan dan minimal satu
   peta lain. Pilih jump point dari daftar `tools/xc3_tour.py` dan catat argumennya di laporan. Yang dilihat:
   pipeline waits dan frame >100 ms di awal adegan atau pertarungan.
5. **Pemeriksaan visual.** Hold dibuat untuk mencegah draw menunggu pipeline di tengah frame (T240: kepala atau
   rambut hilang 1–2 frame di awal adegan). Periksa apakah ada gejala itu di B. Kalau tour tidak menyimpan
   screenshot, sebutkan di laporan dan minta user memeriksa satu awal adegan secara manual.
6. **Kriteria berhasil (B dibanding A):**
   - Loading hold selama tour turun jelas dari ~6–8 detik, dan total loading hold sepanjang run ikut turun.
   - Pipeline waits dan program waits tidak naik berarti (tetap dalam ratusan ms).
   - Frame >100 ms dan worst frame tidak lebih buruk.
   - Warm tidak berubah dibanding `p0_warm_b`.
   - Kalau pipeline waits naik tajam di langkah 4, **berhenti dan laporkan**: berarti program tanpa varian memang
     digambar di rute itu, dan solusinya memperluas seed (1.3 di `plan_perf_stutter.md`), bukan tebakan.
7. **Sesudah berhasil:**
   - `graphify update .`, lalu smoke tour sekali lagi dengan build final.
   - Tambahkan hasilnya ke `docs/plan_perf_stutter.md` sebagai Phase 1, butir baru sesudah 1.0, dengan tabel angka
     A / B / B-warm. Tambahkan juga satu baris di `docs/ROADMAP.md` bila ada bagian perf di sana.
   - **Laporkan dan tunggu persetujuan user sebelum commit.** Isi commit hanya: perubahan `exec.c`, dua dokumen
     tadi, dan dua file rencana (`docs/plan_precompile_driver_cache.md`, `docs/plan_skip_guess_prebuild.md`).
   - Jangan commit patch probe, exe tambahan (`build/xc3_*.exe`), atau isi `build/tour_runs/`.

## Aturan

- Save selalu salinan scratch lewat `LARECOMP_SAVE_DIR`.
- Setiap boot di folder `play/xc3_bootN` baru dengan junction `targets`.
- Run tersembunyi tidak menulis `cache/vk_pipelines.bin`; jangan set `XCDE_SAVE_CACHE=1`.
- `cache/variants.bin` milik pemain ikut menentukan program mana yang "tanpa varian". Untuk run dingin yang
  sebanding, pakai `cache/variants.bin` yang sama di setiap run: salin file itu sebelum run pertama dan kembalikan
  sebelum setiap run.
- Jangan menebak perintah, flag, atau lokasi: kalau tidak ada di repo atau dokumentasinya, tanyakan ke user.

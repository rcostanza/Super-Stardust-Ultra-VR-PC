# Super Stardust Ultra VR PC runner

Native Windows and Linux runner for the PS4 version of Super Stardust Ultra VR (CUSA05951 1.00;
Housemarque's own engine, statically linked into eboot.bin, FMOD audio in SD3_bank*.fev/.fsb with
mp3 music, data in stream.dat). The game's x86-64 code runs directly; PS4
system calls are reimplemented in C and GNM rendering goes through shadPS4's video core on Vulkan.
See [docs/development.md](docs/development.md) for the pipeline and [docs/building.md](docs/building.md)
for the toolchain and packaging. No game files are in the repository, and nothing assumes where the
decrypted dump is: set `STARDUST_GAME_DIR` to the game (or pass `--game` to `run.py`). Tests that
need it skip without it.

## Loop
- Build: `bash build.sh` (Git Bash on Windows, where it puts MSYS2 CLANG64 on PATH itself; or Linux).
- Run: `python run.py --timeout 20`; read the first `STOP:` or `Fault:` line.
- Missing imports: `python run.py check` (full list in `out/game/missing-imports.txt`).
- Tests: `bash build.sh --test` (runtime C tests and every CMake `*-test`), then
  `cd tests && python -m unittest discover -p "test_*.py"`. `build_windows.ps1 -Test -Run` /
  `build_linux.sh --test --run` do the build, both suites and a boot check in one go.
- Scripted input for boot checks past the menus: `STARDUST_PAD_FILE` (button names, re-read when
  the file changes); `tools/screenshot.ps1` captures the window.

## Conventions
- Names are Super Stardust Ultra VR's: identifiers, files, env variables (`STARDUST_*`), log prefixes and
  patch markers in vendored code (`// stardust:`).
- C runtime imports resolve by symbol name: a function goes in a `src/runtime_*.c` export table
  (`{"name",function}`) and its NID in `src/import_names.inc`. Name NIDs with
  `python tools/name_imports.py out/game --write` (candidates in `tools/ps4_names.txt`).
- `libScePosix` imports are looked up under the `libkernel` key (runtime.c, `canonical_name`).
- POSIX variants return errno; `sce*` variants return SCE codes (0x8002xxxx, errno in the low
  16 bits). See `posix_result` in runtime_mutex.c.
- The loader builds with `-Wall -Wextra -Werror`. Match the surrounding terse C style and comment
  each function with the PS4 contract it follows.
- Guest-visible pointers must stay below 1 TiB (`runtime_low_alloc`).
- Keep only what this game needs: no extra menus or hacks for other games. The renderer
  is shadPS4's with added performance work (draw preparation, memos, async pipelines); behaviour
  changes there need a boot check (`python run.py --timeout 20`).
- Releases are built by `tools/package.py` and must work from the unpacked zip alone: no repo, no
  MSYS2, no Python of the player's own. Never package game files or anything built from a dump.
- Auto-setup (src/launcher.c, src/prepared_stamp.h): stardust.exe starts stardust-setup (window,
  and exits) when paths.ini is missing or its game folder has no eboot.bin, and runs
  `stardust-setup --patch` (progress window) and waits when game/release.txt, the stamp
  `release=` / `dump=<eboot sha256>` / `eboot=<size> <time>`, doesn't match release.txt and the dump
  (size+time first, SHA-256 only when they differ). A dump whose eboot.bin isn't the tested 1.00 one
  (`untested_warning`, setup.cpp) asks to continue in the window and in --patch; the CLI wants
  `--accept-untested`.
- RecompLauncher: `recomplauncher.json` (repo root, shipped at the top of every package by
  tools/package.py; archives are `-windows-x64.zip` / `-linux-x64.tar.gz`) prepares with
  `stardust-setup --game {game} --progress` and edits settings.ini; keep its settings in step with
  settings.ini keys. `RECOMPLAUNCHER=1` makes stardust.exe log to save/log as when unattended.
- Nothing is written outside the project or release folder: shader caches go to `shaders/`
  (`run.py` `shared_cache_dir`, `src/launcher.c`), paths to `paths.ini` beside the exe.
- Python edits on Windows: write files with LF line endings (`write_bytes`, or
  `open(path, 'w', newline='')`): text mode would turn the tree's LF into CRLF.
- No update or DLC: only the base 1.00 dump is supported. sceAppContentGetAddcontInfo answers no
  entitlement for every add-on label.
- Game-specific code (eboot 1.00 addresses, `Build` table keyed by SHA-256) lives in
  `src/runtime_game.c`: frame clock global at image +0x9b6250 (set_fps = vtable slot 3 at 0x53fe40,
  rate at +8, step at +0xc); main loop 0x426e10 is a fixed-step accumulator on real time.
- Above 60 FPS, logic tuned per update keeps its 59.94 Hz timing: `rescale_tunables` rescales the
  per-update `sd3__` tunables (`tunables_0100`: lerp, mul, per-update amount, frame count,
  countdown that fires after 0: (n+1)/r-1, e.g. menu key repeat); `update_constants_0100` does the
  same for per-update multipliers that are rip-relative constants (ship velocity damping 0x31cd76/
  0x31cdbd, patch data +8 on);
  `patch_particles` runs the particle task (start hook on vtable slot 0x700a90) at 59.94 Hz, with
  step-0 runs in between because a frame whose task didn't run draws no particles, and the streak
  multiply at 0x489349 reading the last full step (patch data +0); `patch_scene_step` makes the
  renderer's per-frame scene step (0x422e1a/0x422e80, was 1/59.94) 1/rate (patch data +4);
  `patch_page_fade` makes the menu page fade (UI_Page +0x104 lerp 0.7, multiply at 0x392628)
  0.7^r (patch data +0x40). `x = target + (x-target)*k` keeps k (MUL, e.g. `sd3__menu__*_decay`), not LERP. For a new
  "faster/shorter at high FPS" report, find the tunable or constant applied per update or per frame
  (docs/development.md, Frame rate).
- `runtime_game_patch` (before the game runs): render size immediates (movabs 0x41ee1b/0x556899,
  movl 0x2623ca/0x2623d5) follow `render_resolution` (720p/1080p/1440p/4K, all native), and the
  PS VR headset wait (sceKernelSleep calls at 0x556e5c/0x556ea2) is NOPed.
- `skip_intro=1`: logos fast-forwarded (`runtime_game_in_intro`), D3T movie ended after its first
  frame (`runtime_pad_logo_movie`), Cross pressed for the usage terms and on the title when
  `menu.mp3` opens (`runtime_game_opened`). Boot to main menu ~10 s.
- Pad speaker (sceAudioOut type 4, one mono port): `pad_route` (runtime_audio.c) plays it on a USB
  DS4/DualSense audio device (name match, re-checked ~1 s), else main output at PAD_SPEAKER_GAIN;
  a wired DualSense also gets output path 3 (speaker) via SDL_SendGamepadEffect on the pad host
  thread (`speaker_path`, runtime_pad.c), restored on close/exit. settings.ini `pad_speaker`
  controller/main/off. No Bluetooth audio.
- FMOD's profiler polls accept on a non-blocking socket every frame: net_accept/recv must not sleep
  for SO_NBIO sockets (that bug held the game at 16 FPS).
- No HDR: the game imports sceVideoOutGetDeviceCapabilityInfo but always opens SDR; no `hdr` setting.
- Aspect: only 16:9 is drawn (`runtime_game_widen` returns 0), so there is no aspect setting (an old
  `aspect_ratio` key is ignored); windows take 16:9 sizes, other screens get bars. Exclusive
  fullscreen never switches to a mode of another shape than the desktop's (`PickFullscreen`): a
  16:9 mode on an ultrawide gets stretched by the monitor/GPU scaler.
- The overlay's pages (Display Settings, exit prompt) share `gpu/shim/stardust_page.h` (the game's
  menu look: chrome gold type, orb cursor, glowing pale blue frames); type is Bruno Ace (OFL,
  `gpu/third_party/fonts`), the free face nearest the game's. Never embed the game's own fonts.
  The Windows setup paints the same look in GDI (setup/main.cpp, font as an RCDATA resource).
  `STARDUST_PAD_FILE` token `settings=<row>` opens the panel for screenshots. `settings/` builds settings.exe.
- Display Settings opens with F10 or Select/Start held 1 s (`menu_hold`, runtime_pad.c; taps of
  either are held back and replayed on release) and freezes the game while open: the game thread
  waits at its flip and audio output waits (`runtime_game_wait_frozen`), the present thread
  redraws the last frame (`ImGui::Core::MustKeepDrawing`), and the hang dump ignores it.
- Cheats (L3+R3 / X+M toggle the overlay's Cheats page, checkboxes, paused like Display Settings;
  saved as settings.ini `cheats` bits; Triangle unticks all): `patch_cheats` in runtime_game.c hooks Player_Master_Impl update (slot 0x707438):
  god_mode tunable 0x9be37c, respawn lives code 0x4d386d/0x4d387a, smart bomb `dec` 0x4d5cc4,
  weapon levels +0x98..+0xa0, session (lookup 0x4b03e0) lives +0x84, bombs +0x98, boost cooldown
  +0xb4. docs/development.md "Cheats". Pad file: `cheats`, `cheats=<bits>`.
- Local co-op (runtime_pad.c): two gamepad slots on the host thread, handle n = user n. A second
  pad's first connection logs in user 2 (`runtime_pad_users` -> runtime_services.c user list/event),
  which the game's pad manager (0xac60) answers with scePadOpen(2). The keyboard and the pad file are
  player 1's.
- The vendored renderer draws point-list draws with `gl_PointCoord` for the I,J barycentrics
  (`HwFragmentRuntimeInfo::point_sprite`), inherited from earlier work; keep it unless it breaks.

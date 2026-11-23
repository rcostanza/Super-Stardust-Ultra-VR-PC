# Development

How the runner works and how to work on it. Building, tests and packaging are in
[building.md](building.md).

## How it works

The PS4 and the PC are both x86-64, so nothing is recompiled. Super Stardust Ultra VR's `eboot.bin` (Housemarque's
engine, statically linked, with FMOD for audio: `SD3_bank*.fev`/`.fsb` and mp3 music) is mapped into memory and run directly. What
differs is everything that code calls:

| The game calls | Replaced by |
|---|---|
| PS4 system libraries (kernel, POSIX, threads, files, pad, audio, save data, ...) | C reimplementations in `src/runtime_*.c` |
| GNM GPU command buffers | shadPS4's video core in `gpu/`, GCN shaders translated to Vulkan |

The game image is built from the dump by `run.py` (and by the setup in a release).

1. `scripts/prepare.py` strips the SELF wrapper from `eboot.bin` (the dump must be decrypted) and
   writes `out/game/boot.bin`, its report `analysis.json` and the AppContent profile `content.bin`.
2. `scripts/link.py` links the eboot with the two modules Super Stardust Ultra VR ships in `sce_module`, `libc.prx`
   and `libSceFios2.prx`, into `boot-linked.bin`. Symbols match by NID and library identity, never by
   name; `mov rax, fs:[0]` thread-pointer loads are rewritten to GS (glibc owns FS on Linux).
3. On a CPU without an instruction set the game uses (MOVBE, LZCNT, BMI1), `scripts/isa_rewrite.py`
   lists each such instruction for `src/isa_rewrite.c`, which patches in a stub that emulates it.
4. `stardust` maps the image, resolves every import against the C runtime (by symbol name, from
   `src/import_names.inc`) or the GPU core (by NID), runs each module's initializer and enters the
   game. An import with no implementation becomes a trap that prints
   `STOP: first unsupported PS4 import: <name>` and exits.

Every import Super Stardust Ultra VR's three modules make is implemented, and only those: a runtime function the
game does not import is not kept. `python run.py check` lists any that are missing (none today).

The renderer draws at the game's own resolution (`render_resolution`, 720p to 4K, see below) and
scales the picture to the window in `post_process.frag`. With `sharpening=1` (the default) that pass draws into an image of the
window's size and FSR 1 RCAS (`rcas.frag`, `host_passes/rcas_pass.cpp`) sharpens it into the frame.
The only overlay is the PS4's system UI (save list, text entry, trophy notices), the Display Settings
panel and the exit prompt (`gpu/shim/stardust_overlay.cpp`).

## Running from the source tree

Set `STARDUST_GAME_DIR` to your decrypted dump (or pass `--game DIR` each time):

```bash
python run.py --timeout 20
```

- Without a command, `run.py` prepares `out/game/` if the dump or `scripts/` changed and starts the
  game. The first `STOP:` or `Fault:` line it prints is the next thing to fix. `--timeout` ends the
  run after that many seconds.
- `python run.py check` lists the imports the runtime cannot resolve, by library (full list in
  `out/game/missing-imports.txt`). `python run.py prepare` rebuilds the image only.
- `--guest-arg SWITCH` passes a command-line argument to the game.
- Saves go to `user/` (`--user DIR`), settings to `user/settings.ini`. Shader caches go to `shaders/`
  in the project folder (`STARDUST_GPU_USER_DIR` moves it).
- `STARDUST_SAMPLE_MS=<ms>` (Windows) logs the main thread's stack every that many milliseconds,
  for finding where a slow frame spends its time.
- Every `settings.ini` key has an environment variable that overrides it, named in
  `settings.example.ini`. A variable left set in a shell keeps overriding the panel:
  `STARDUST_VBLANK_HZ` prints a warning at start for that reason.
- `STARDUST_VERBOSE=1` logs every routine event (threads, semaphores, memory maps, module loads).
  `run.py` and the debug package's `diagnose/start.bat` turn it on.
- Hang dump: once a frame has been shown, a picture held for `STARDUST_HANG_DUMP` seconds (default 20,
  `0` turns it off) logs every thread's name, CPU time and frames (`Hang:` lines, innermost first).
  The `--timeout` watchdog prints the same list before it ends a run. The dump starts with how far
  the GPU got on the rasterizer and sparse-binding timelines.
- `STARDUST_AUDIO_LEVELS=1` logs each audio port's peak levels every 3 s (the first three
  minutes); the exit report keeps the session totals either way.
- GPU crashes: a lost device prints the driver's fault report (`GPU fault:` lines,
  VK_EXT_device_fault). `STARDUST_GPU_CHECKPOINTS=1` (NVIDIA) also puts a checkpoint before every
  recorded command and turns on the driver's own diagnostics; the report then names the call site
  and shaders of the last commands each queue reached (`GPU checkpoint` lines, also in a hang dump).
  `STARDUST_SYNC_DISPATCH=1` submits and waits after every compute dispatch, so a fault names the
  compute shader it happened in (`GPU fault: during the dispatch of compute shader 0x...`) or says
  it was outside every dispatch (a draw or copy). Slow; diagnosis only.
- `bash tools/mash_mission.sh <secs> <log>` boots straight into the first mission, shooting.

## Development loop

1. `python run.py --timeout 20` and read the first `STOP:` or `Fault:` line.
2. Implement the function in the matching `src/runtime_*.c` export table (`{"name",function}`),
   following the PS4 contract, with a comment that states it. Return "not available" for services a
   PC does not have (PSN, the camera, the companion app).
3. `bash build.sh`, run again.

When an import shows up as a bare NID, add candidate names to `tools/ps4_names.txt` and run
`python tools/name_imports.py out/game --write`: it hashes every candidate (and every name the
runtime already implements) and appends the matches to `src/import_names.inc`. For a fault in guest
code, `python tools/image_info.py flatten <module> out.bin` writes a module as a flat image to
disassemble at the offset the log names, and `python tools/image_info.py got <offset>` names the
import bound to a GOT slot.

## Frame rate

The engine's main loop (eboot 1.00, 0x426e10) is a fixed-step accumulator on real time: each frame it
runs as many updates of one step as the elapsed time holds (at most about four, then it drops
time), and the step comes from the frame clock, the global at image +0x9b6250 (vtable 0x70dfe8;
`set_fps`, slot 3, at 0x53fe40, keeps the rate at +8 and the step, 1/rate, at +0xc). The boot sets
59.94.

`frame_rate` (30, 60, 90, 120, 144 or 180) sets it; above about 220 enemies stop spawning, so 180 is the top (an old `frame_rate=240` reads as 180). `StardustSettings::EffectiveFrameRate()`
lowers it to what the display refreshes at, unless VSync is off with tearing allowed; the video out's
vblank follows it (`GetVblankFrequency`, live), and at each flip `runtime_game_frame_rate()`
(`src/runtime_game.c`) calls the engine's own `set_fps` (60 becomes the game's 59.94). Before its
first change the runner checks the clock: a vtable in the image's read-only data, `set_fps` in its
code and a target near 59.94 or 29.97; otherwise it leaves the rate alone and says so in the log.
Each step then covers less time, so the game's speed stays the same. An eboot other than 1.00 (the
`Build` table, by SHA-256) gets none of the game patches.

Much of the game's logic ignores the step it is given and was tuned for one update every 1/59.94 s:
`x += (target-x)*k` lerps, `v *= m` decays and frame counts run once per update. Above 60 FPS that
made the menus move faster, the Gold Melter's beam shorter and tighter, and the ship's boost and the
enemies' turning quicker. Those constants are the engine's named tunables (`sd3__...` strings; each
is a global struct registered at static init, its value at +0xc), so `rescale_tunables`
(`src/runtime_game.c`, table `tunables_0100`) rescales the ones used per update at each flip, for
r = 59.94/rate: a lerp k becomes 1-(1-k)^r, a multiplier m^r, a per-update amount s*r, a frame count
n/r. Check which side a lerp writes: `x = target + (x-target)*k` (the menus' `_decay` and `_iir_`
tunables) keeps k of the distance, so it is a multiplier (m^r), not a lerp; rescaled as a lerp it
ran the menu transitions slow at 30 FPS and near instant at 240. The game's own value is kept and taken again whenever the game writes a new one; at 60 FPS the
originals are restored. The table was built by reading each tunable's use (lerps, `_mul`, `_decay`,
`_frames`, frictions); one that uses the step already, or isn't a rate, stays out.
Countdowns that fire on the update after they reach 0 take n+1 updates, so they become (n+1)/r-1
(`COUNTDOWN_INT`/`COUNTDOWN_FLOAT`; the float one gets +0.5 because the game truncates it): the
menus' held-direction key repeat (`sd3__menu__keyrepeat__delay_cycles` and the "to" end of
`repeat_count_to_cycles`, used at 0x36e2b0) is one, and repeated 4x as fast at 240 FPS.

A few per-update multipliers are plain constants instead of tunables: `update_constants_0100`
lists their `vmovss xmmN,[rip+...]` loads, which `patch_update_constants` points at patch data
floats (from +8) that `rescale_update_constants` keeps at m^r. The player ship's velocity damping
(0.95 and 0.9, 0x31cd76 and 0x31cdbd in the ship update 0x31b6b0) is there: per update, it cut the
boost's dash short above 60 FPS.

Particles (the plasma shots and their trails, sparks, debris, explosions) are authored for 59.94
simulation runs a second; run once per update at 240 FPS the shots' trails were a quarter as long,
and scaling single step reads in the worker (0x486770) didn't bring them back: it is the simulation
as a whole. `patch_particles` hooks the task's start (`Particle_Simulation_Task_Impl` vtable slot 2
at 0x700a90, target 0x489600) so it simulates at 59.94 Hz: a full run with the steps added up once
1/59.94 s of nominal time has passed, a run with step 0 otherwise. The task can't simply be skipped:
the same run builds that frame's particle vertices (from a per-frame pool, with the frame's camera),
and a frame whose task didn't run draws no particles at all. Step 0 moves and spawns nothing (the
worker never divides by the step) but redraws. Streak length is speed times step (the vertex
builder's multiply at 0x489349, `[rbp-0x7dc]`), so that multiply reads a float in the loader's
patch data page instead, set to the last full run's step. The cadence is kept per task, by address,
in a 64-entry table that hands the least recently started entry to a new task: the game makes and
frees tasks as effects come and go, and two tasks sharing one entry stalled one effect and then ran
it at double speed.

Menu page changes fade the outgoing page out and the incoming one in: `UI_Page`'s update (0x3925e0)
does `alpha = target + (alpha-target)*k` once per update with k a page field (+0x104) that both
constructors set to 0.7, and `Menu_Impl` drops the old page once its alpha is under 0.01. That made
a page change 4 updates long: 133 ms at 30 FPS, 17 ms at 240. `patch_page_fade` points the multiply
(`vmulss xmm2,xmm2,[rdi+0x104]` at 0x392628) at a patch data float (+0x40) that `apply_frame_rate`
keeps at 0.7^r. The page's rotation beside it uses `sd3__menu__movement_decay` the same way.

The renderer (`Game_Renderer_Impl_ST` vtable slot 2, 0x422d50) advances its scenes' animations
once per drawn frame by a constant 1/59.94 (`vmovss xmm0,[0x5b90f4]` at 0x422e1a and 0x422e80),
which made the main menu's camera drift and background run at rate/60 speed. `patch_scene_step`
points both loads at a patch data float that `apply_frame_rate` keeps at 1/rate.

The game ran at 16 FPS until `sceNetAccept` stopped sleeping: FMOD's profiler polls `accept` on a
non-blocking socket from the main thread every frame, so non-blocking sockets (`SO_NBIO`, or
`MSG_DONTWAIT`) now return `EWOULDBLOCK` at once (`src/runtime_services.c`).

### Render resolution

The engine's render device is 1920x1080 on PS4 and PS4 Pro alike, and its render script sizes every
target from it. `runtime_game_patch` (`src/runtime_game.c`, before the game's code runs) rewrites the
four immediates that hold that size (two packed `movabs` at 0x41ee1b and 0x556899, two `movl` at
0x2623ca/0x2623d5) to the `render_resolution` (720p, 1080p, 1440p or 4K; 16:9), checking the bytes
first. The game then draws everything at that size, HUD and menus included. PS4 Pro mode
(`sceKernelIsNeoMode`) changes nothing in this game.

### Aspect ratios

Only 16:9 is drawn. A wider render device was tried: the presenter still letterboxes it and the
menus' text stretches, so `runtime_game_widen` reports "not supported" and there is no aspect ratio
setting: windows take 16:9 sizes and fullscreen on another shape shows the 16:9 picture with bars.
Exclusive fullscreen keeps a display mode of the screen's own shape (`PickFullscreen`) and the
presenter draws the bars: switching an ultrawide to a 16:9 mode let the monitor or GPU scaler stretch
it across the panel.

### HDR

None: the game asks `sceVideoOutGetDeviceCapabilityInfo` but always opens an SDR output, so there is
no `hdr` setting.

### Shader compiler

Super Stardust Ultra VR's compute shaders add an immediate 0 to a `ds_consume` result before taking the lane
offset off it, which shadPS4's append/consume lowering
(`RemoveAppendBufferLaneOffset`, `resource_patching_pass.cpp`) asserted on when the first mission
loaded. The no-op add is folded away and the match runs again; arithmetic on the counter that is not
the lane offset is left as it is.

## Starting and leaving

The boot first waits about 5 s for a PS VR headset (`sceKernelSleep(2)` at 0x556e5c and
`sceKernelSleep(1)` at 0x556ea2); `runtime_game_patch` removes both calls. With `skip_intro=1` (the
default) the Sony, XDEV and Housemarque logos are fast-forwarded (`runtime_game_in_intro`: the frame
loop's base time moves 50 ms per flip), the D3T movie (`d3t_1080.mp4`) is ended by AvPlayer after
its first frame (`runtime_pad_logo_movie`), and Cross is pressed for the usage terms and once more on
"PRESS X BUTTON" when the menu music (`menu.mp3`) opens (`runtime_game_opened`). A start lands in the
main menu in about 10 s.

F4, or Select+Start, asks "Exit Super Stardust Ultra VR?". The prompt is drawn by the overlay and
takes the input while open; Yes pushes the same quit event as closing the window. Holding Select
(SDL's Back button) for two seconds opens the Display Settings panel (F10 too): the button's own
press is held back while it is down, and a release before two seconds sends it to the game as a
short tap. `settings.exe` (`settings/`) edits the same `settings.ini` outside the game.

## Cheats

L3+R3 (both sticks clicked; X+M on the keyboard, `cheats_combo` in `src/runtime_pad.c`) opens or
closes the Cheats page (`stardust_overlay.cpp`, checkbox rows in `stardust_page.h`), which pauses
the game as Display Settings does. The ticked cheats are bits (`stardust_cheats()`), saved as
`settings.ini` `cheats` when the page closes; Triangle (H, Delete) unticks them all. `patch_cheats` (`src/runtime_game.c`, eboot 1.00) hooks `Player_Master_Impl`'s update
(vtable slot 2 at 0x707438, target 0x4d1fa0) and applies them from there, on the game's thread:

| Cheat | How |
|---|---|
| Invulnerability | the engine's own `sd3__game__god_mode` tunable (value at 0x9be37c) set to 1; the ship's hit handler (0x4d2f40) checks it |
| Infinite lives | the respawn (0x4d37c0) keeps the session's lives (`Game_Session_Impl` +0x84): `lea ecx,[rax-1]` at 0x4d386d becomes `lea ecx,[rax]`, and the `jg` at 0x4d387a past the last-ship notice a `jmp`; at least 1 kept |
| Infinite bombs | the smart bomb (0x4d5cb6) no longer `dec`s the session's bombs (+0x98) at 0x4d5cc4; at least 1 kept, so it works when turned on with none |
| Max weapons | `Player_Master_Impl` +0x98/+0x9c/+0xa0 held at `sd3__game__maximum_weaponN_power` - 1, the game's own top level |
| Instant boost | the session's boost cooldown (+0xb4, 4 s, counted down by the session update 0x2c5972) zeroed |

The session is the game's own lookup by id (0x4b03e0) of `Player_Master_Impl` +0xac. The code
changes are written (protection lifted for the write) when a cheat changes, and every site's bytes
are checked at start; `STARDUST_PAD_FILE` takes `cheats` (open the page) and `cheats=<bits>`
(1 invulnerable, 2 lives, 4 bombs, 8 weapons, 16 boost) for scripted runs. Smart bombs fire on
L2+R2.

## Save data, add-ons and trophies

Saves are folders under `<save>/savedata/<user>/CUSA05951/` (`src/runtime_savedata.c`). The game
mounts them with the SDK 1-3 `sceSaveDataMount` as well as `sceSaveDataMount2`. The save list dialog
(`runtime_services.c`) lists them newest first.

The game asks `sceAppContentGetAddcontInfo` about each add-on label it knows; none is installed,
so every one answers no entitlement (`src/runtime_content.c`).

Trophies come from `sce_sys/trophy/trophy00.trp`; unlocks are kept in `<save>/trophies.dat` and shown as a notice.

Online features (PSN, leagues, matchmaking, leaderboards) see a signed-out console: contexts fail
with NP SIGNED_OUT, which the game handles as offline play. `libSceJson2` documents the game builds
for its PSN requests (`sce::Json::Array`, `Object` and `Value`, `src/runtime_json.c`) work in memory.

## Release folder and setup

`stardust-setup.exe` (`setup/`) runs `prepare/run.py prepare` into `game/` beside it, rewrites the
game's instructions for this CPU if needed, writes `settings.ini` from the template when there is
none, sets its `skip_intro` line from the window's checkbox, and writes `paths.ini` with the game and
save folders. Nothing is written outside the release folder:
shader and pipeline caches go to `shaders/` beside the exe (`cache=` moves them). The command line does the same as the window:
`stardust-setup.exe --help`.

Started with no arguments, `stardust.exe` reads `paths.ini` and builds the command line `run.py`
would pass (`src/launcher.c`); without a console its output goes to `log/stardust.log` in the save
folder. It also watches the game: it starts the same command line as a child (on Windows in a job
that ends it if the launcher is killed, on Linux by `fork` with `PR_SET_PDEATHSIG`), and when the
game ends with an exit code other than 0 it notes the code in the log, copies the log to
`log/crash-<date>-<time>.log` and says so in a message box. `STARDUST_CRASH_WATCH=0` turns that off. Started by RecompLauncher (`RECOMPLAUNCHER=1`), it does the
same even when given a pipe for its output.

Before that it keeps the folder set up by itself. Without a usable `paths.ini` (missing, or its game
folder has no `eboot.bin`) it starts `stardust-setup` and exits. Otherwise it compares
`game/release.txt`, the stamp the setup writes (`src/prepared_stamp.h`: `release=`, `dump=` the
SHA-256 of the dump's `eboot.bin`, `eboot=` its size and time), with `release.txt` and the dump; the
size and time are checked first and the SHA-256 only when they differ. When the release or the dump
changed it runs `stardust-setup --patch`, which prepares `game/` again from `paths.ini` in a progress
window, and starts the game once that succeeds. A dump whose `eboot.bin` isn't the tested 1.00 one
(`untested_warning`, `setup/setup.cpp`) makes the setup window and `--patch` ask before going on; the
command line needs `--accept-untested`.

Preparing the game needs Python 3.10 or newer with its standard library only; releases bundle
python.org's embeddable Python in `prepare/python`. The CPU rewrite also needs `iced-x86`, which only
CPUs missing an instruction set ever use. The icon of both exes is `res/stardust.ico`
(`tools/make_app_icon.py` builds it from artwork).

## Input

The pad and keyboard layouts are fixed tables in `src/runtime_pad.c` (README "Controls"): gamepads map
by button position, keys by physical position. `stardust --gamepad` prints the connected pad and its
labels.

Local co-op: the game's pad manager (eboot 0xac60, 8 slots) polls `sceUserServiceGetEvent` and opens a
pad with `scePadOpen(user)` for each LOGIN, as a PS4 does when a second controller signs in. The host
thread keeps two gamepad slots (first found is player 1, plus the keyboard; second is player 2), and a
pad keeps its slot while connected. The second pad's first connection logs in user 2 ("Player 2", red;
`runtime_pad_users`, `runtime_services.c`), and user 2 stays logged in after that. Its pad then reads
disconnected until a gamepad fills the slot again. Pad handle n is user n's. Scripted input (pad file,
record, replay) and the intro presses are player 1's only.

Scripted input for automated runs: `STARDUST_PAD_FILE` (held buttons and stick values, re-read when
the file changes), `STARDUST_PAD_RECORD` (F9 records the pad) and `STARDUST_PAD_REPLAY`.

## Layout

| Path | What it is |
|---|---|
| `src/` | Loader (`loader.c`), launcher (`launcher.c`), the runtime (`runtime_*.c`) and the CPU rewrite (`isa_*.c`) |
| `gpu/` | shadPS4's video core (`shadps4/`, changes marked `// stardust:`), the shim and overlay around it (`shim/`), CMake build |
| `build.sh`, `build_windows.ps1`, `build_linux.sh` | The build (`build.sh`) and the local build, test and boot-check wrappers ([building.md](building.md)) |
| `scripts/` | Building the game image: `prepare.py`, `link.py`, `isa_rewrite.py` |
| `setup/` | `stardust-setup`, which sets up a release folder from a player's dump |
| `settings/` | `settings.exe`, the game's options outside the game |
| `res/` | The exes' icon, Windows resource script and manifests (`libs.manifest`: the DLL folder) |
| `packaging/` | `diagnose/`, the glitch kit the debug package ships |
| `.github/` | CI (`workflows/build.yml`) and its scripts: `ci-deps.sh` (Linux build dependencies), `smoke-test.sh` (a release on fresh distros) |
| `tools/` | `package.py` (the release), `name_imports.py` (names NIDs), `image_info.py` (modules and GOT slots), `make_app_icon.py`, `screenshot.ps1` |
| `tests/` | Python tests (`cd tests; python -m unittest`) and C/C++ tests (`build.sh --test`: its own list and every `*-test` in `gpu/CMakeLists.txt`) |
| `docs/` | This guide and [building.md](building.md) |

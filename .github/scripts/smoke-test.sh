#!/usr/bin/env bash
# Checks an unpacked Linux release (tools/package.py) on a distro it was not built on. Run as root in
# a fresh container of that distro (.github/workflows/build.yml): it installs what a desktop already
# has (python3, the Vulkan loader, the X11/Wayland/audio libraries the release leaves to the host),
# then checks without a game dump or a GPU:
#   - the programs find every library (ldd) and start: a missing one or a too-new glibc stops them;
#   - the game, never set up, starts stardust-setup; stardust-setup rejects a folder that is not a
#     dump, warns about stand-in files that aren't the tested dump, and with them accepted
#     (--accept-untested) runs prepare/run.py through the system python3 to its end;
#   - a set-up folder (paths.ini, game/ with its stamp) starts the game from it, logging to
#     <save>/log; with a stale stamp the game runs stardust-setup --patch first, which, with nobody
#     to confirm the untested stand-in files, stops, and the game does not start.
# Usage: smoke-test.sh <unpacked release folder>
set -euo pipefail
release=$(readlink -f -- "$1")
if command -v apt-get >/dev/null; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends python3 libvulkan1 \
        libx11-6 libx11-xcb1 libxcb1 libxcb-render0 libxcb-shm0 libxext6 libxfixes3 libxrender1 libxi6 libxcursor1 libxrandr2 \
        libwayland-client0 libwayland-egl1 libwayland-cursor0 libxcb-shape0 libxcb-xfixes0 libasound2t64 libpulse0 libudev1 \
        libfontconfig1 libfreetype6 zlib1g libdrm2 libgbm1 libegl1 libgl1 libstdc++6 >/dev/null
elif command -v dnf >/dev/null; then
    dnf install -y -q python3 vulkan-loader libX11 libX11-xcb libxcb libXext libXfixes libXrender libXi \
        libXcursor libXrandr libwayland-client libwayland-egl libwayland-cursor alsa-lib pulseaudio-libs \
        systemd-libs fontconfig freetype zlib-ng-compat libdrm mesa-libgbm libglvnd-egl libglvnd-glx \
        libstdc++ >/dev/null
elif command -v pacman >/dev/null; then
    pacman -Sy --noconfirm --needed python vulkan-icd-loader libx11 libxcb libxext libxfixes libxrender \
        libxi libxcursor libxrandr wayland alsa-lib libpulse systemd-libs fontconfig freetype2 zlib \
        libdrm mesa libglvnd gcc-libs >/dev/null
fi
grep -m1 PRETTY_NAME /etc/os-release
ldd --version | sed -n 1p  # head would end ldd with SIGPIPE (pipefail)
fail() { echo "FAIL: $*" >&2; exit 1; }
# Without a display, as in a container: started without a terminal, the game and the setup would
# otherwise show their messages in a box and wait for it to be closed.
unset DISPLAY WAYLAND_DISPLAY
cd "$release"

for exe in stardust stardust-setup; do
    [[ -x $exe ]] || fail "$exe is missing or not executable"
    if ldd "$exe" | grep 'not found'; then fail "$exe: libraries missing (above)"; fi
done
status=0
./stardust --cpu-features || status=$?
[[ $status == 0 || $status == 20 ]] || fail "stardust --cpu-features exited $status"
usage=$(./stardust-setup --help 2>&1 || true)
grep -q 'usage: stardust-setup' <<<"$usage" || fail "stardust-setup --help has no usage: $usage"

work=$(mktemp -d)
# Not set up: no paths.ini. No terminal (as from a file manager): the game hands over to the setup,
# which, with no display either, says so on stderr.
rm -f paths.ini
status=0
./stardust </dev/null >"$work/unset.log" 2>&1 || status=$?
grep -q 'The game is not set up yet' "$work/unset.log" || { cat "$work/unset.log"; fail 'no "not set up" message'; }
grep -q 'usage: stardust-setup' "$work/unset.log" || { cat "$work/unset.log"; fail 'the game did not start stardust-setup'; }
[[ $status != 0 ]] || fail "stardust without paths.ini (and no display for the setup) exited 0"

# A folder that is not a dump, then one that has the files the setup looks for (empty, so the
# preparation itself fails, in python3).
mkdir -p "$work/empty" "$work/dump/sce_sys" "$work/dump/sce_module"
status=0
./stardust-setup --game "$work/empty" --save "$work/save" >"$work/setup-empty.log" 2>&1 || status=$?
grep -q "isn't a decrypted Super Stardust Ultra VR dump" "$work/setup-empty.log" ||
    { cat "$work/setup-empty.log"; fail 'the setup took a folder without eboot.bin'; }
for f in eboot.bin sce_sys/param.sfo sce_module/libc.prx sce_module/libSceFios2.prx sce_module/libSceNpToolkit.prx SD3_bank00.fev stream.dat; do
    : >"$work/dump/$f"
done
status=0
./stardust-setup --game "$work/dump" --save "$work/save" >"$work/setup-version.log" 2>&1 || status=$?
grep -q "haven't been tested with this release" "$work/setup-version.log" ||
    { cat "$work/setup-version.log"; fail 'the setup took untested files without --accept-untested'; }
[[ $status == 1 ]] || fail "stardust-setup with untested files exited $status"
status=0
./stardust-setup --game "$work/dump" --save "$work/save" --accept-untested >"$work/setup-dump.log" 2>&1 || status=$?
cat "$work/setup-dump.log"
grep -q 'Preparing the game from' "$work/setup-dump.log" || fail 'the setup did not start preparing'
grep -q 'Preparing the game failed (exit code' "$work/setup-dump.log" || fail 'the preparation did not run to its end'
grep -q '^  ' "$work/setup-dump.log" || fail "no output of prepare/run.py reached the setup's log"
[[ $status == 1 ]] || fail "stardust-setup with an empty dump exited $status"
[[ ! -e paths.ini ]] || fail 'a failed setup wrote paths.ini'

# Set up by hand, with the stamp the setup writes (src/prepared_stamp.h): the game starts from
# paths.ini and logs to <save>/log (no terminal). The boot file is not a real one, so it stops there.
mkdir -p game
printf 'not a boot file' >game/boot-linked.bin
: >game/content.bin
printf 'release=%s\ndump=%s\neboot=%s\n' "$(head -n1 release.txt)" "$(sha256sum "$work/dump/eboot.bin" | cut -d' ' -f1)" \
    "$(stat -c '%s %.9Y' "$work/dump/eboot.bin")" >game/release.txt
printf '[paths]\ngame=%s\nsave=%s\n' "$work/dump" "$work/save" >paths.ini
status=0
./stardust </dev/null >"$work/launch.out" 2>&1 || status=$?
log=$work/save/log/stardust.log
[[ -f $log ]] || { cat "$work/launch.out"; fail "no $log"; }
cat "$log"
grep -q "Launch: game $work/dump" "$log" || fail 'the game did not start from paths.ini'
(( status != 0 && status < 128 )) || fail "stardust exited $status on a bad boot file (0 or a signal)"
grep -q 'preparing the game' "$work/launch.out" "$log" && fail 'a current stamp prepared the game again'

# The same eboot.bin copied (a new time, the same contents): still current, by its SHA-256.
touch -d '2001-01-01' "$work/dump/eboot.bin"
status=0
./stardust </dev/null >"$work/copy.out" 2>&1 || status=$?
grep -q 'preparing the game' "$work/copy.out" "$log" && fail 'the same eboot.bin with a new time prepared the game again'

# A stale stamp (as after unpacking a newer release): the game runs stardust-setup --patch, which
# warns about the untested stand-in files and, with nobody to confirm, stops; the game does not start.
printf 'release=an older one\n' >game/release.txt
status=0
./stardust </dev/null >"$work/patch.out" 2>&1 || status=$?
cat "$work/patch.out"
grep -q 'Launch: preparing the game' "$work/patch.out" || fail 'a stale stamp did not run stardust-setup --patch'
grep -q "haven't been tested with this release" "$work/patch.out" || fail 'stardust-setup --patch did not warn about untested files'
grep -q 'Launch: game' "$work/patch.out" && fail 'the game started after a refused --patch'
[[ $status == 1 ]] || fail "stardust after a refused --patch exited $status"
rm -rf game paths.ini

echo "PASS: $(grep -m1 PRETTY_NAME /etc/os-release | cut -d= -f2)"

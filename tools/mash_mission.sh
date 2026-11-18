#!/usr/bin/env bash
# Boot the game and drive it into Barrow, Alaska > Blood on the Ice with the save in user/:
# taps Cross every 0.3 s and holds R2/L2 (shooting) from launch. Stops at the first STOP:,
# Fault: or "Hang: no new frame" line, or after $1 seconds. Log: $2.
# Needs STARDUST_GAME_DIR; extra STARDUST_* env passes through.
# Usage (Git Bash): bash tools/mash_mission.sh 90 out/mash.log
limit=${1:-180}; log=${2:-out/mash.log}
cd "$(dirname "$0")/.."
export STARDUST_PAD_FILE="$PWD/out/mash-pad.txt"
: > "$STARDUST_PAD_FILE"
python -u run.py --timeout "$limit" > "$log" 2>&1 &
pid=$!
start=$(date +%s)
while kill -0 $pid 2>/dev/null; do
    echo cross > "$STARDUST_PAD_FILE"; sleep 0.1
    echo r2 l2 > "$STARDUST_PAD_FILE"; sleep 0.2
    if grep -qE "^STOP:|^Fault:|^Hang: no new frame" "$log"; then break; fi
    [ $(( $(date +%s) - start )) -gt $((limit + 30)) ] && break
done
sleep 3
kill $pid 2>/dev/null; taskkill //F //IM stardust.exe >/dev/null 2>&1
grep -nE "GPU fault|GPU checkpoint|STOP:|Fault:|Assertion" "$log" | head -20

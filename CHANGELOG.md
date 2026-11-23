# Changelog

Each release's section becomes the text of its GitHub release (`tools/release_notes.py`, run by
`.github/workflows/build.yml` when a `vN.N` tag such as `v0.1` is pushed). Add the section before
tagging: a tag without one does not publish. Keep each entry on one line: GitHub shows a line break
in release text as a break.

## v0.2

- Fullscreen on ultrawide monitors now shows black bars at the sides instead of stretching the picture
- Selecting Interactive Streaming in the Single Player menu no longer crashes the game (the mode needs PlayStation streaming, so it does nothing)
- Add support for Dualshock 4 and Dualsense speakers. To disable it, set the controller volume to 0 in Settings > Audio.

## v0.1

- First release: Super Stardust Ultra VR (PS4, CUSA05951) running natively on Windows and Linux, from your own decrypted dump

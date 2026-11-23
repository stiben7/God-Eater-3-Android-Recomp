<p align="center"><img src="docs/icon.png" width="128" alt=""></p>

# God Eater 3 Android Recomp

An **experimental** native Android port of the Switch version of **God Eater 3**, made from **your own copy** of the
game. It isn't an emulator: the game's own ARM64 code runs straight on the phone's CPU, and this project rewrites the
parts of the console it talks to (files, threads, input, audio, and the NVN graphics API on top of Vulkan).

> **Status: early work in progress. The game does not boot yet.** The first goal is to get past the game's start-up and
> find out which Switch SDK calls and graphics features it needs that the runtime doesn't provide yet.

> **Free, unofficial fan project.** Not affiliated with, endorsed or sponsored by Bandai Namco or Nintendo. All
> trademarks belong to their owners. This repository has **no game code and no game assets**: you need a copy of the
> game you bought, with its files extracted from your own console.

## Where this comes from

The runtime is taken from [stiben7/MH-Rise-Android-Recomp](https://github.com/stiben7/MH-Rise-Android-Recomp), a
Snapdragon 8 Gen 2 build of [klark231/mhrise-recomp](https://github.com/klark231/mhrise-recomp), itself based on
[IamWilmar's port](https://github.com/IamWilmar/mhrise-recomp). Its loader and its Switch OS and NVN-to-Vulkan layers are
not specific to one game; the game-specific parts (build ID, file markers, frame clock) are being replaced here.

## Target

- Game: God Eater 3, title ID `0100 1C70 0873 E000`, base version 1.0.0, built with Switch SDK 6.6.0.
- Device used for development: Snapdragon 8 Gen 2 (Adreno 740), Odin 2 Portal.
- Requires the extracted **ExeFS** and **RomFS** of your own copy. `main`'s build ID starts `3887159ca9306230`.

## Building

See the build section of the original project: Android SDK with NDK 28.2.13676358, JDK 17+, CMake 3.22+, then
`tools/setup.sh` and `tools/build_apk.sh`. The GitHub Actions workflow builds an APK on every push to `main`. Each CI
build is signed with a new debug key, so installing a newer one over an older one needs an uninstall first.

## License

GPL-3.0, like the code it comes from. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.

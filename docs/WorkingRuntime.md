# Accepted runtime and development checkout

## Combined Matroska AAC source inspection, 2026-11-22

The accepted DLL exports `AveMediaBridge_InspectMatroskaAacSourceV1`. On an eligible video MKV it hashes the pinned file while proving the selected AAC-LC track's frame count. It publishes both values only after checking the relevant CRCs, track mapping, continuity and physical EOF. If stronger existing evidence applies or the scan cannot establish the result, the caller uses its ordinary import path. Cancellation and non-success leave the output arguments untouched. [Five-file receipt](../tools/runtime/accepted-runtime-20261122.json)

The exact qualified DLL and four FFmpeg libraries are archived under `out/runtime-baselines/2026-11-22-combined-mkv/`. Its evidence directory contains the original Release build log and cache, an archive of the exact sources used, per-file qualification results, and the tests from the production source checkout. The 175-file source archive is identified in the receipt. With line endings normalized, 102 of its 103 files under `src/` and `include/` match the recorded production source commit exactly. The remaining public header differs only in its description of the optional export. The production build includes that export by default. The earlier qualification build enabled it explicitly with the same compiler, linker inputs for the new files, and pinned FFmpeg package.

At qualification, the user recording contained 70,286,336 decoded frames per channel. A generated positive control and a copy missing one interior AAC block exercise the combined proof and clean fallback. The regression test recalculates the cluster CRC on the edited fixture, so the missing interval still has to be detected. It also checks cancellation, independence of the file cursor, and SHA-256 against a separate reader. The complete Release CTest run passed 18 tests and skipped nine historical external-fixture suites. Previous October and November 18 runtime snapshots are retained.

## Guarded progressive imports, 2026-11-18

The five production file sizes and SHA256 hashes are recorded in [accepted-runtime-20261118.json](../tools/runtime/accepted-runtime-20261118.json). The Bridge DLL was built from `3394d2a95dcc05755efa444b9ff6fc07cc1eaf60` with the external FFmpeg 7.1.4 `candidate-002` package. All four FFmpeg DLLs retain their October hashes. The output layout is `AveVoice/build/product/Release/Modules` and `Lib/ffmpeg`.

For a fresh editing workspace a consumer may explicitly request the guarded path or reader import entry point. On supported local NTFS a read of `AveMediaBridge_GuardedPcmFileIdV1` on the import callback thread identifies the current append-only file. The caller must pin and check the matching file handle, consume only committed frames, and verify the final identity and byte count before adopting a progressive fingerprint. A missing identity requires reading the completed file. The existing path and reader import APIs retain their original output contract.

Qualification compared the October DLL directly with this DLL on the same stereo AAC/MP4. Probe JSON, both import sidecars and all 768,000 PCM bytes matched exactly. A 2,048-frame Float32 WAV export preserved the format and every sample, including values above unity. A pre-existing change in the development branch (`b4a37ad`) omits the generated encoder metadata from the WAV container. Import and export smoke tests ran with the four pinned FFmpeg libraries. The native guard regression test covers cancellation, a nested legacy import, an occupied output name, write access after close and exact PCM samples. Additional Windows probes checked write access in a second process before and after writer close and exercised the guarded reader on real media.

The previous five-file baseline remains available under `out/runtime-baselines/2026-10-27-working/`. The new local five-file snapshot follows the same layout under `out/runtime-baselines/2026-11-18-guarded/`.

## October 2026 runtime and preserved backups

On 2026-10-27 the user accepted the currently working AveVoice runtime. The user subsequently requested that the existing development state from `AveMediaBridge_audio_export` continue under the ordinary `AveMediaBridge` folder name, after complete backups, with the deployed runtime left unchanged.

### October binary set

[`accepted-runtime-20261027.json`](../tools/runtime/accepted-runtime-20261027.json) records the sizes and SHA256 hashes of exactly five files: `Modules/AveMediaBridge.dll` and the four FFmpeg DLLs under `Lib/ffmpeg`, relative to `AveVoice/build/product/Release`.

The October Bridge hash starts with `B26FB5CB`. Its historical source reference is `b293466`; the installed binary hash is authoritative. A newer commit or a successful rebuild does not automatically replace this accepted binary set. The FFmpeg set is the existing qualified 7.1.4 `candidate-002` runtime with Float32 WAV export support.

A verified local copy is kept in this checkout at `out/runtime-baselines/2026-10-27-working/`, using the same `Modules/` and `Lib/ffmpeg/` layout, with a copy of the manifest. This directory is ignored by Git. Preserve it during local artifact cleanup; do not commit its binaries.

The manifest and its copied snapshot remain unchanged. Its `development` fields describe the checkout and source checkpoint when the runtime was pinned; the current development location is specified below.

## Development workflow

- Make future changes in `AveMediaBridge`, on `main`. The transferred development checkpoint is `e3f1c2a` (production source checkpoint: `b4a37ad`), with no additional production-code changes during the folder transfer. The October baseline retained the previously deployed binary at the time of that transfer.
- The former `codex/audio-export` branch is retained as a history checkpoint. Its separate working folder is retired after verification. Complete archives of both original folders, including hidden Git metadata, local binaries, and build outputs, are kept at `out/workspace-backups/layout-20261027/`; per-file inventories and archive hashes accompany them. Preserve these ignored local backups.
- Build and test locally. Do not copy a development DLL or FFmpeg libraries into AveVoice as part of routine build, testing, or folder cleanup.
- Replacing the accepted runtime requires a new explicit user request. Before an authorized replacement, retain the accepted five-file set and verify the candidate against the user's actual import/export workflow.
- Do not change the accepted manifest merely to match a new build. A changed baseline must record a separately accepted runtime.

The AveVoice executable is outside this five-file baseline and is not included in the local snapshot.

## Local development build

The active development dependency is the same external `candidate-002` package previously used by `AveMediaBridge_audio_export`. The retained `ffmpeg/` directory is the older local package. Configure a fresh build explicitly from the repository root:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DAVEMEDIABRIDGE_FFMPEG_ROOT="../ffmpeglab/research/audio_export_foundation_20260928/candidate-002/install"
cmake --build build --config Release
cmake -E env AMBE_EXPECT_FLOAT_WAV=1 ctest --test-dir build -C Release -j1 --output-on-failure
```

The former CMake cache is preserved with the full backups. Generated files containing the old absolute source path are not reused. The optional reader laboratory roots remain empty because that historical corpus is unavailable; available container and reader-media-facts fixtures can still be configured independently. None of these commands invokes AveVoice deployment.

# Accepted runtime and development checkout

On 2026-10-27 the user accepted the currently working AveVoice runtime. The user subsequently requested that the existing development state from `AveMediaBridge_audio_export` continue under the ordinary `AveMediaBridge` folder name, after complete backups, with the deployed runtime left unchanged.

## Accepted binary set

[`accepted-runtime-20261027.json`](../tools/runtime/accepted-runtime-20261027.json) records the sizes and SHA256 hashes of exactly five files: `Modules/AveMediaBridge.dll` and the four FFmpeg DLLs under `Lib/ffmpeg`, relative to `AveVoice/build/product/Release`.

The accepted Bridge hash starts with `B26FB5CB`. Its historical source reference is `b293466`; the installed binary hash is authoritative. A newer commit or a successful rebuild does not automatically replace this accepted binary set. The FFmpeg set is the existing qualified 7.1.4 `candidate-002` runtime with Float32 WAV export support.

A verified local copy is kept in this checkout at `out/runtime-baselines/2026-10-27-working/`, using the same `Modules/` and `Lib/ffmpeg/` layout, with a copy of the manifest. This directory is ignored by Git. Preserve it during local artifact cleanup; do not commit its binaries.

The manifest and its copied snapshot remain unchanged. Its `development` fields describe the checkout and source checkpoint when the runtime was pinned; the current development location is specified below.

## Development workflow

- Make future changes in `AveMediaBridge`, on `main`. The transferred development checkpoint is `e3f1c2a` (production source checkpoint: `b4a37ad`), with no additional production-code changes during the folder transfer. This remains a development candidate, separate from the accepted deployed binary set.
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

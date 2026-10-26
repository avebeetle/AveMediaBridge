# Accepted runtime and development checkout

On 2026-10-27 the user accepted the currently working AveVoice runtime and requested that development continue in `AveMediaBridge_audio_export`, with both working folders retained and the deployed DLL left unchanged.

## Accepted binary set

[`accepted-runtime-20261027.json`](../tools/runtime/accepted-runtime-20261027.json) records the sizes and SHA256 hashes of exactly five files: `Modules/AveMediaBridge.dll` and the four FFmpeg DLLs under `Lib/ffmpeg`, relative to `AveVoice/build/product/Release`.

The accepted Bridge hash starts with `B26FB5CB`. Its historical source reference is `b293466`; the installed binary hash is authoritative. A newer commit or a successful rebuild does not automatically replace this accepted binary set. The FFmpeg set is the existing qualified 7.1.4 `candidate-002` runtime with Float32 WAV export support.

A verified local copy is kept in this checkout at `out/runtime-baselines/2026-10-27-working/`, using the same `Modules/` and `Lib/ffmpeg/` layout, with a copy of the manifest. This directory is ignored by Git. Preserve it during local artifact cleanup; do not commit its binaries.

## Development workflow

- Make future changes in `AveMediaBridge_audio_export`, on `codex/audio-export` (source checkpoint at pin: `b4a37ad`). This remains a development candidate, separate from the accepted deployed binary set.
- Retain the separate `AveMediaBridge` checkout. Do not merge, remove, or rename either working folder without a new explicit user request.
- Build and test locally. Do not copy a development DLL or FFmpeg libraries into AveVoice as part of routine build, testing, or folder cleanup.
- Replacing the accepted runtime requires a new explicit user request. Before an authorized replacement, retain the accepted five-file set and verify the candidate against the user's actual import/export workflow.
- Do not change the accepted manifest merely to match a new build. A changed baseline must record a separately accepted runtime.

The AveVoice executable is outside this five-file baseline and is not included in the local snapshot.

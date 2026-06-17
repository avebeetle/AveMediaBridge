# AveMediaBridge Professional Architecture Review

Status: architecture recommendation only. No production code movement in this stage.

## Executive Summary

Recommended architecture name: Layered C-ABI Adapter With Probe/Decode/Import Services.

AveMediaBridge should evolve from a single large DLL implementation into a small exported C ABI adapter over cohesive internal services. The public ABI stays stable. FFmpeg remains a private implementation detail. The most important domain rule to preserve is the frame-count trust contract: AveMediaBridge must distinguish authoritative decoded frame counts from unsafe estimates, and AveVoice must continue to use that distinction to separate final source length from provisional Loading extent.

The refactor should not replace one giant file with one giant `MediaBridgeManager`. The goal is a few narrow services with explicit data models:

```text
DLL adapter -> Probe service -> Packet scan + FrameCountPolicy -> Probe JSON
DLL adapter -> Streaming import service -> Decode/resample + progress/waveform chunks -> session artifacts
DLL adapter -> legacy transform/export path
```

The first safe extraction after the current RAII helper stage should be stream selection and FFmpeg runtime helpers. Frame-count policy should move only after packet scan data is stable and regression comparisons are easy to run.

## Current Project Shape

Current build targets from `CMakeLists.txt`:

- `AveMediaBridgeCore`: static library with existing public C++ core/import/export/process helpers.
- `AveMediaBridge`: DLL module currently built from `src/Dll/AveMediaBridgeDll.cpp` and linked to `AveMediaBridgeCore`.
- `AveMediaBridgeLabApp`: console smoke/manual tool linked to the core library and dependent on the DLL.

Current source layout:

```text
include/AveMediaBridge/
  AveMediaBridge.hpp
  AveMediaBridgeApi.hpp
  Core/
  Export/
  Import/
  Process/

src/
  AveMediaBridge.cpp
  Core/
  Diagnostics/
  Dll/
  Export/
  Ffmpeg/
  Import/
  Process/
```

The new `src/Ffmpeg/FfmpegHeaders.hpp` and `src/Ffmpeg/FfmpegDeleters.hpp` are a good first step because they isolate FFmpeg include exposure and ownership helpers without changing behavior.

## Public ABI Review

`include/AveMediaBridge/AveMediaBridgeApi.hpp` is the C ABI contract. It must remain stable for AveVoice and external smoke tools.

Currently exported functions:

- `AveMediaBridge_TransformToWav`
- `AveMediaBridge_GetVersionString`
- `AveMediaBridge_GetLastErrorText`
- `AveMediaBridge_ProbeToJson`
- `AveMediaBridge_ProbeFrameCountCandidatesToJson`
- `AveMediaBridge_ImportAudioToSession`
- `AveMediaBridge_ImportAudioToSessionEx`
- legacy `AveMediaBridge_GetVersion`

Currently exported callback/data structs:

- `AveMediaBridgeImportProgress`
- `AveMediaBridgeWaveformChunk`
- `AveMediaBridgeImportOptions`
- progress/cancel/waveform callback typedefs

AveVoice currently requires these exports at load time:

- `AveMediaBridge_GetLastErrorText`
- `AveMediaBridge_GetVersionString`
- `AveMediaBridge_ProbeToJson`
- `AveMediaBridge_ImportAudioToSession`

AveVoice resolves these as optional:

- `AveMediaBridge_TransformToWav`
- `AveMediaBridge_ImportAudioToSessionEx`
- `AveMediaBridge_ProbeFrameCountCandidatesToJson`

Despite being optional in AveVoice, existing public exports should not be removed during this refactor. `ProbeFrameCountCandidatesToJson` can later be documented as diagnostic/internal-support ABI, but changing or removing it should be a separate deliberate compatibility decision.

### Must Remain Stable

- Exported symbol names and calling convention.
- Struct field order, field size, and `structSize` extension behavior.
- Return-code meanings, especially `0` success and `AVEMEDIABRIDGE_IMPORT_RESULT_CANCELED`.
- Thread-local last-error retrieval behavior.
- Required probe JSON fields consumed by AveVoice, especially `decodedSampleFramesTrust`, `decodedSampleFramesSource`, `decodedSampleFramesKind`, and `frameCountPolicyReason`.
- Session artifact names: `metadata.json`, `audio_info.json`, `original_f32.bin`.
- Progress callback monotonicity and waveform chunk borrowed-pointer lifetime.

### Safe To Move Internally

- FFmpeg deleters, runtime helpers, and include wrapping.
- Stream selection helpers.
- Probe value structs, packet scan structs, frame-count policy structs.
- Decode/resample helpers.
- JSON writing helpers.
- Path validation and atomic artifact commit helpers.
- Progress reporter and waveform chunk emitter internals.

### Must Never Cross The C ABI Boundary

- FFmpeg types such as `AVFormatContext`, `AVCodecContext`, `AVPacket`, `AVFrame`, `SwrContext`, `AVChannelLayout`, `AVStream`, `AVCodecParameters`.
- C++ ownership types such as `std::string`, `std::wstring`, `std::vector`, `std::filesystem::path`, `std::unique_ptr`, `std::function`.
- Exceptions, RAII objects, or allocator-owned memory that callers must free.

## AveMediaBridgeDll.cpp Responsibility Map

The current file is doing all of these jobs:

- Exported API adapter: exported functions, input validation, `try`/`catch`, last-error text, return-code translation.
- FFmpeg integration: FFmpeg includes, error formatting, channel layout formatting, codec/container helpers.
- FFmpeg lifetime: raw object cleanup and the newly extracted RAII helpers.
- Stream selection: best audio stream and first-audio fallback.
- Probe/preflight: fast probe, stream/source metadata, estimated bytes, disk preflight.
- Packet scan/candidates: skip-sample side data, packet counts, packet duration sums, packet PTS spans.
- Frame-count policy: all codec/container trust decisions and JSON evidence fields.
- Decode/resample: decoder open/send/receive, frame layout, `SwrContext`, converted float32 output.
- Progressive import: session artifact creation, incremental writes, temp files, commit/rollback.
- Waveform/progress callbacks: draft min/max and energy chunks, throttled progress, cancel polling.
- JSON serialization: probe JSON, metadata JSON, audio info JSON, arrays, warnings, errors.
- Logging/errors/diagnostics: last-error text, FFmpeg error strings, full-scale clipping diagnostic env var.

The file has enough internal structure to extract safely, but only if each extraction keeps the data boundaries explicit.

## Recommended Target Architecture

Recommended source layout, adapted to the current project:

```text
include/AveMediaBridge/
  AveMediaBridgeApi.hpp       # stable C ABI only
  AveMediaBridgeTypes.hpp     # optional future POD aliases only, no FFmpeg/C++ ownership
  AveMediaBridge.hpp          # existing public C++ facade for LabApp/core workflows
  Core/
  Export/
  Import/
  Process/

src/Dll/
  AveMediaBridgeDll.cpp       # thin exported adapter

src/Core/
  MediaBridgeContext.hpp/.cpp # internal service wiring, no global God object
  MediaBridgeError.hpp/.cpp   # error values and FFmpeg error wrappers
  Result.hpp                  # small internal Result<T>/Status helper if useful

src/Ffmpeg/
  FfmpegHeaders.hpp           # only private FFmpeg include point
  FfmpegDeleters.hpp          # RAII deleters and unique aliases
  FfmpegRuntime.hpp/.cpp      # version/runtime helpers if needed
  StreamSelection.hpp/.cpp    # selected audio stream rules
  PacketReader.hpp/.cpp       # sequential packet read wrapper if it simplifies scans/import

src/Probe/
  MediaProbeService.hpp/.cpp  # fast probe orchestration
  ProbeTypes.hpp              # FastProbeResult, stream summaries if internal
  PacketScan.hpp/.cpp         # gapless + packet timeline scan
  FrameCountPolicy.hpp/.cpp   # trust/candidate decision logic
  ProbeJsonWriter.hpp/.cpp    # probe JSON only

src/Decode/
  AudioDecodeService.hpp/.cpp # decoder send/receive loop coordination
  AudioResampler.hpp/.cpp     # SwrContext and float32 conversion
  PcmFormat.hpp/.cpp          # format/channel/sample layout helpers

src/Import/
  StreamingImportService.hpp/.cpp
  ImportArtifactWriter.hpp/.cpp
  WaveformChunkEmitter.hpp/.cpp
  ProgressReporter.hpp/.cpp

src/Utils/
  PathUtils.hpp/.cpp
  JsonUtils.hpp/.cpp
  Logging.hpp/.cpp
```

Do not create all files at once. Add each file only when real behavior moves into it and the build/test delta is small.

## Module Responsibilities

### Dll

`AveMediaBridgeDll.cpp` should become boring. It should:

- validate public input pointers and output buffers;
- create internal request structs from C ABI parameters;
- call internal services;
- catch exceptions;
- set thread-local last-error text;
- return stable public result codes.

It should not know packet PTS math, codec-specific frame policy, `SwrContext` details, JSON field formatting, or session artifact commit mechanics.

### Ffmpeg

`src/Ffmpeg` should own FFmpeg exposure and lifetime:

- all FFmpeg includes through `FfmpegHeaders.hpp`;
- all FFmpeg object deleters;
- optional thin runtime/version helpers;
- stream-selection rules that require FFmpeg stream/codec types;
- optional packet reader helpers for repeated read/unref loops.

This layer should still be concrete. Hiding FFmpeg behind vague abstract interfaces would make debugging worse.

### Probe

`src/Probe` should own no-decode source metadata and frame-count trust:

- `MediaProbeService` opens input, gathers metadata, calls stream selection, runs packet scans, applies frame-count policy, and returns a value model;
- `PacketScan` produces evidence only;
- `FrameCountPolicy` decides trust only;
- `ProbeJsonWriter` writes JSON only.

Keep JSON formatting separate from policy. Policy should be testable without string output.

### Decode

`src/Decode` should own decoded audio production:

- decoder context setup;
- send/receive loops;
- resampler setup and flush;
- converted interleaved float32 output chunks.

This can be class-based when RAII state is meaningful. Avoid hiding all decode behavior behind an abstract base class; this project has one FFmpeg backend.

### Import

`src/Import` should own streaming session artifact production:

- `StreamingImportService` coordinates probe/decode/write/progress/cancel;
- `ImportArtifactWriter` owns temp files, final renames, data-size verification, and cleanup;
- `ProgressReporter` owns callback throttling and monotonic progress snapshots;
- `WaveformChunkEmitter` owns chunk binning, min/max, RMS/mean-abs evidence arrays, and borrowed-pointer callback lifetime.

Import should consume decoded sample chunks. It should not contain frame-count candidate selection.

## OOP And Design Guidance

Use classes where there is real lifetime/state:

- FFmpeg context wrappers if `unique_ptr` aliases are not enough.
- `AudioResampler` for `SwrContext`, source layout, output layout, and output buffer reuse.
- `ImportArtifactWriter` for temp/final paths and file handles.
- `ProgressReporter` for last-emitted frame count and final progress state.
- `WaveformChunkEmitter` for pending bins and callback-local arrays.

Use data-only structs for evidence and results:

- `ProbeResult`
- `PacketScanResult`
- `GaplessSkipSampleScan`
- `FrameCountDecision`
- `FrameCountCandidate`
- `StreamingImportResult`
- `DecodeChunk`

Use free functions for pure, deterministic transforms:

- sample-rate-aware codec frame size helpers;
- timestamp-to-frame conversion;
- container/codec predicate checks;
- frame-count candidate sanity checks;
- JSON escaping if kept simple.

Use stateless services sparingly:

- `MediaProbeService` and `StreamingImportService` can be concrete classes when they group dependencies and make APIs clear.
- Do not build a `MediaBridgeManager` that owns every service and every state. That would recreate the current God file in object form.

Dependency injection is useful for:

- file/path operations in tests;
- clock/progress callback throttling if tests need determinism;
- a packet-scan input model for frame-count policy tests.

Dependency injection is overengineering for:

- replacing FFmpeg with an interface when no second backend exists;
- abstracting tiny math helpers;
- wrapping every JSON write operation in a class hierarchy.

## Ownership And Lifetime Model

### FFmpeg Objects

- `AVFormatContext`: owned by a `UniqueAVFormatContext` or a narrow context class. Close with `avformat_close_input` only in the FFmpeg ownership layer.
- `AVCodecContext`: owned by `UniqueAVCodecContext` or `DecoderSession`. Free with `avcodec_free_context`.
- `AVPacket`: owned by `UniqueAVPacket` or stack-local packet reader. Each read loop must unref after each packet.
- `AVFrame`: owned by `UniqueAVFrame` or decode session. Reused across receive loop.
- `SwrContext`: owned by `AudioResampler` or `UniqueSwrContext`. Recreate only when input layout/format/sample-rate changes.

### Callbacks And Progress

- Callbacks remain synchronous on the import thread.
- Callback pointers and `userData` remain borrowed from the caller.
- Waveform chunk arrays remain valid only during callback execution.
- Progress should publish only data that is already written/flushed enough for AveVoice to consume.
- Cancel polling should remain frequent but must clean artifacts deterministically.

### Buffers And Artifacts

- Streaming import should keep bounded buffers and never allocate the full decoded file.
- Legacy core import/export may still use `AudioBufferF32` where those workflows intentionally need full in-memory audio.
- `original_f32.bin` remains the stable streaming artifact.
- `metadata.json` and `audio_info.json` remain final commit gates.

### Probe JSON And Value Model

- Probe services should return values first, write JSON second.
- JSON field names should be locked by tests before moving writers.
- `decodedSampleFramesTrust/source/reason` are not cosmetic; AveVoice uses them for Loading geometry.

## Error And Result Model

Internal code should use a small result/status model with:

- machine-readable category/code;
- human-readable message;
- optional warning list;
- optional FFmpeg error code and string.

Public API still exposes:

- integer return code;
- thread-local last-error text;
- JSON warnings/errors for probe/import details.

Fatal errors:

- invalid ABI input pointer or invalid output path;
- cannot open source media;
- no usable audio stream for import;
- decoder/resampler setup failure;
- write/flush/commit failure;
- data-size mismatch at final commit;
- caller cancellation, returned as cancel code.

Warnings:

- non-authoritative frame count estimate;
- FFmpeg stream-info imperfections when selected audio remains valid;
- generated MPEGPS bogus-stream noise with stable selected MP2 frame count;
- CAF/ALAC exact no-decode count unavailable;
- skipped invalid packets when decode still completes.

Logging should be compact. Normal import/probe should not dump frame-count candidates unless a diagnostic API or explicit log level requests it.

## Frame Count Policy Architecture

Frame-count policy should be a dedicated `src/Probe/FrameCountPolicy` component. It should not live in JSON writer, streaming import, or decode code.

Recommended data flow:

```text
MediaProbeService
  -> StreamSelectionResult
  -> PacketScanResult + GaplessSkipSampleScan
  -> FrameCountPolicy::decide(input)
  -> FrameCountDecision
  -> ProbeResult
  -> ProbeJsonWriter
```

Recommended value types:

```text
FrameCountCandidate
  name
  frames
  trustCandidate
  source
  reason
  evidence flags

FrameCountPolicyInput
  formatName
  codecId/codecName
  sampleRate
  streamDurationFrames
  formatDurationFrames
  durationEstimateFrames
  packet scan evidence
  gapless scan evidence

FrameCountDecision
  decodedSampleFrames
  decodedSampleFramesKind
  decodedSampleFramesTrust
  decodedSampleFramesSource
  decodedSampleFramesBeforeCorrection
  packetPtsSpanFrames
  packetDurationSumFrames
  packetFrameCountCandidateUsed
  frameCountPolicyReason
  gapless fields
```

Codec/container rules to preserve:

- MP3: two-sided gapless skip/padding; bare MP3 one-sided start skip only for `formatName == "mp3"` plus MP3 codec; MP3-in-MP4 packet-count-minus-skip only when packet-derived evidence is exact.
- AAC: ADTS packet-derived count; MPEG-TS/M2TS/MTS packet PTS span with valid full scan.
- Opus: gapless skip/discard or exact packet timeline for Ogg/Matroska/WebM family.
- MPEGPS MP2: packet count times MP2 frame size when sane.
- WMAV2: ASF/WMA/WMV packet count minus one decoder delay frame, with sample-rate-aware frame length.
- CAF/ALAC: remain non-authoritative unless an exact no-decode candidate is proven.

This policy should be unit-testable with synthetic `FrameCountPolicyInput` values. Full media import should not be required to test decisions such as bare MP3 one-sided skip versus MP3-in-MP4 one-sided skip rejection.

## Testing Strategy

### Unit-Style Policy Tests

Add pure tests for:

- MP3 two-sided gapless correction;
- bare MP3 one-sided start skip accepted;
- MP3-in-MP4 one-sided duration-estimate correction rejected;
- MP3-in-MP4 packet-count-minus-skip accepted;
- ADTS AAC packet duration/count accepted;
- Opus skip/discard accepted;
- MPEG-TS AAC packet PTS span accepted even when packet duration sum is poor;
- MPEGPS MP2 packet count accepted;
- WMAV2 frame size at 8000/16000/22050/44100/48000;
- CAF/ALAC unsafe estimate preserved.

### Packet Scan / Probe Tests

Use real files for packet scan behavior because timestamp quirks are container-specific:

- `D:\2.mp3`
- `tests\output.mp3`
- `tests\28_mp4_h264_mp3_stereo_44100.mp4`
- `tests\46_mpegts_h264_aac_stereo_44100.ts`
- `tests\58_flv_h264_mp3_stereo_44100.flv`
- 40-minute ADTS AAC, Opus, WMAV2 targets when available locally.

Compare probe JSON fields, not just success/failure.

### Corpus Tests

Keep three tiers:

- short corpus smoke: fast broad compatibility over `AveMediaBridge\tests`;
- targeted 40-minute speech smoke: long-duration natural media for high-risk formats;
- sample-rate matrix: focused MP3/WMAV2/ADTS AAC/PCM variants.

Known regression targets:

- `D:\2.mp3`: bare MP3 one-sided skip exact authoritative, no Ready shrink.
- `output.mp3`: two-sided MP3 gapless exact.
- MP4+MP3: no unsafe duration-estimate one-sided correction; packet-count-minus-skip allowed when exact.
- ADTS AAC: packet-derived count avoids bitrate-estimate failure.
- Opus: exact final decoded frames, no 1024-frame reconcile.
- TS AAC: packet PTS span prevents end growth.
- WMAV2: packet count minus decoder delay, sample-rate aware.
- CAF/ALAC: warning/non-authoritative, not fake exact.
- MPEGPS: frame count stable; generated bogus-stream probe noise can remain warning.

## Staged Refactor Plan

### Stage 1: FFmpeg RAII And Include Boundary

Goal: complete the current low-risk extraction of FFmpeg includes/deleters.

Files likely changed:

- `src/Ffmpeg/FfmpegHeaders.hpp`
- `src/Ffmpeg/FfmpegDeleters.hpp`
- `src/Dll/AveMediaBridgeDll.cpp`

Risk: low.

Expected behavior change: none.

Validation:

```bat
cmake --build build --config Release
```

Rollback: inline the small deleters/includes back into `AveMediaBridgeDll.cpp`.

Commit point: after Release build and diff review.

### Stage 2: Stream Selection Helpers

Goal: move best-audio and first-audio fallback rules into `src/Ffmpeg/StreamSelection`.

Files likely changed:

- `src/Ffmpeg/StreamSelection.hpp/.cpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Risk: low to medium.

Expected behavior change: none.

Validation:

```bat
cmake --build build --config Release
```

Run smoke probe/import for one normal MP4 and one generated MPEGPS sample.

Rollback: remove new files from CMake and restore local helper functions.

Commit point: after selected stream index is unchanged for representative files.

### Stage 3: Packet Scan Value Model

Goal: move `GaplessSkipSampleScan`, `PacketFrameCountScan`, and packet scan loops into `src/Probe/PacketScan`.

Files likely changed:

- `src/Probe/PacketScan.hpp/.cpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Risk: medium.

Expected behavior change: none.

Validation:

```bat
cmake --build build --config Release
```

Compare scan-derived probe JSON fields for MP3, MP4+MP3, ADTS AAC, Opus, TS AAC, MPEGPS MP2, WMAV2.

Rollback: route calls back to old local scan functions.

Commit point: after packet evidence fields match before/after.

### Stage 4: FrameCountPolicy Extraction

Goal: move trust decision logic into `src/Probe/FrameCountPolicy` with pure input/output structs.

Files likely changed:

- `src/Probe/FrameCountPolicy.hpp/.cpp`
- `src/Probe/PacketScan.hpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Risk: highest.

Expected behavior change: none.

Validation:

```bat
cmake --build build --config Release
```

Run policy tests plus probe JSON comparison for every regression target.

Rollback: restore policy functions in the monolith. Do not mix with JSON writer extraction.

Commit point: after short corpus and targeted high-risk files pass.

### Stage 5: ProbeJsonWriter Extraction

Goal: separate JSON formatting from probe policy and probing.

Files likely changed:

- `src/Probe/ProbeJsonWriter.hpp/.cpp`
- `src/Utils/JsonUtils.hpp/.cpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Risk: medium.

Expected behavior change: none.

Validation: semantic JSON comparison before/after; AveVoice Release build.

Rollback: restore old writer functions.

Commit point: after field names/values are unchanged.

### Stage 6: Decode/Resample Service

Goal: move decoder receive loop and `SwrContext` management out of DLL adapter.

Files likely changed:

- `src/Decode/AudioDecodeService.hpp/.cpp`
- `src/Decode/AudioResampler.hpp/.cpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Risk: high.

Expected behavior change: none.

Validation: import WAV/MP3/AAC/Opus/WMA; compare `audio_info.json` frames and `original_f32.bin` size.

Rollback: keep old local implementation until import artifacts match.

Commit point: after real import smoke checks pass.

### Stage 7: Streaming Import Service

Goal: move session artifact lifecycle, progress, cancel, and waveform chunk emission into `src/Import` internals.

Files likely changed:

- `src/Import/StreamingImportService.hpp/.cpp`
- `src/Import/ImportArtifactWriter.hpp/.cpp`
- `src/Import/WaveformChunkEmitter.hpp/.cpp`
- `src/Import/ProgressReporter.hpp/.cpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Risk: high.

Expected behavior change: none.

Validation: progress, waveform, live-readable, and cancel smoke commands; AveVoice progressive open on representative media.

Rollback: route exported function back to old local function.

Commit point: after cancel cleanup and monotonic progress are verified.

### Stage 8: Thin DLL Adapter

Goal: leave `AveMediaBridgeDll.cpp` as public ABI glue only.

Files likely changed:

- `src/Dll/AveMediaBridgeDll.cpp`
- existing service files

Risk: medium after prior stages.

Expected behavior change: none.

Validation: Release build, LabApp smoke, AveVoice Release build.

Rollback: adapter wiring only.

Commit point: when the DLL file is small and behavior is verified.

## What Not To Do

- Do not introduce a giant `MediaBridgeManager` class.
- Do not hide FFmpeg behind vague abstractions that make timestamp/packet debugging harder.
- Do not change the C ABI during internal refactor.
- Do not mix frame-count policy with JSON formatting.
- Do not mix streaming import with frame-count candidate selection.
- Do not move frame-count policy before packet scan evidence is isolated and testable.
- Do not round exact authoritative frame counts upward for internal convenience.
- Do not add fake samples or fake silence to make source lengths match.
- Do not make generated corpus folders source artifacts.
- Do not broaden stream-selection rules while extracting unrelated modules.
- Do not let diagnostic logs become normal import/probe spam.

## Summary Line

```text
[AVEMEDIABRIDGE_PROFESSIONAL_ARCHITECTURE_REVIEW]
recommendedArchitectureName=Layered_C_ABI_Adapter_With_Probe_Decode_Import_Services
publicApiStable=yes
firstRefactorStage=FFmpeg_RAII_and_stream_selection_helpers
highestRiskArea=FrameCountPolicy_and_streaming_import_artifact_lifecycle
filesMostInNeedOfExtraction=src/Dll/AveMediaBridgeDll.cpp
proposedModuleCount=8
recommendedCommitStrategy=commit_after_each_small_behavior-preserving_extraction_with_release_build_and_probe_json_comparison
docsWritten=docs/AveMediaBridge_ProfessionalArchitectureReview.md
sourceChanged=no
behaviorChanged=no
```

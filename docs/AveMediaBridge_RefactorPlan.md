# AveMediaBridge Refactor Plan

Status: staged extraction plan for `src/Dll/AveMediaBridgeDll.cpp`.

## Purpose

`AveMediaBridgeDll.cpp` is currently a large implementation file that mixes exported DLL entrypoints, FFmpeg ownership, probing, packet scanning, frame-count policy, streaming decode/import, JSON writing, progress callbacks, and cleanup.

The target state is a thin exported DLL adapter over small internal services. This plan is explicitly behavior-preserving: public API behavior, import/probe/decode behavior, frame-count policy, JSON fields, artifact names, and progress/cancel semantics must not change during extraction.

## Refactor Principles

- Move one responsibility at a time.
- Prefer pure helpers and value structs before moving FFmpeg runtime code.
- Keep FFmpeg types out of public headers and out of the C ABI.
- Keep the DLL boundary C ABI only.
- Preserve additive JSON fields used by AveVoice.
- Build Release after each extraction stage.
- Compare probe JSON before and after for representative files.
- Keep rollback simple: each stage should be revertible without affecting later production fixes.

## Representative Validation Set

Use a small but meaningful set after each extraction:

```text
D:\2.mp3
D:\rvc\c++\DragonianVoice\AveMediaBridge\tests\output.mp3
D:\rvc\c++\DragonianVoice\AveMediaBridge\tests\28_mp4_h264_mp3_stereo_44100.mp4
D:\rvc\c++\DragonianVoice\AveMediaBridge\tests\46_mpegts_h264_aac_stereo_44100.ts
D:\rvc\c++\DragonianVoice\AveMediaBridge\tests\58_flv_h264_mp3_stereo_44100.flv
D:\rvc\c++\DragonianVoice\AveMediaBridge_tests_40min_speech\04_aac_adts_stereo_44100.aac
D:\rvc\c++\DragonianVoice\AveMediaBridge_tests_40min_speech\07_ogg_opus_stereo_48000.opus
D:\rvc\c++\DragonianVoice\AveMediaBridge_tests_40min_speech\55_asf_wmav2_stereo_44100.asf
```

Generated long-corpus folders are local validation artifacts and must not be committed.

## Stage A: Extract FFmpeg RAII And Stream Selection

Likely files changed:

- `src/Ffmpeg/FfmpegHeaders.hpp`
- `src/Ffmpeg/FfmpegDeleters.hpp`
- `src/Ffmpeg/FfmpegStreamSelection.hpp/.cpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Move:

- FFmpeg include wrapping;
- `AVFormatContext`, `AVCodecContext`, `AVPacket`, `AVFrame`, and `SwrContext` deleters;
- best audio stream and first-audio fallback helpers;
- codec/container name helpers only if they are needed by selection.

Behavior-change risk: low if only ownership wrappers and selection helpers move.

Validation command:

```bat
cmake --build build --config Release
```

Rollback strategy: remove the new files from CMake and restore the moved helper block in `AveMediaBridgeDll.cpp`.

## Stage B: Extract Packet Scan Data Model

Likely files changed:

- `src/Probe/PacketScan.hpp/.cpp`
- `src/Ffmpeg/FfmpegDeleters.hpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Move:

- `GaplessSkipSampleScan`;
- `PacketFrameCountScan`;
- packet side-data parsing;
- packet PTS span, duration sum, packet count, and last-duration calculations.

Behavior-change risk: medium. Packet scan output feeds many frame-count policies.

Validation command:

```bat
cmake --build build --config Release
```

Also compare probe JSON before/after for MP3, MP4+MP3, ADTS AAC, Opus, TS AAC, MPEGPS MP2, and WMAV2.

Rollback strategy: keep the old scan code in the monolith until JSON comparison passes, then delete it in the same small patch.

## Stage C: Extract FrameCountPolicy

Likely files changed:

- `src/Probe/FrameCountPolicy.hpp/.cpp`
- `src/Probe/PacketScan.hpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Move:

- frame-count candidate selection;
- MP3 gapless and one-sided bare MP3 policy;
- MP3-in-MP4 packet-count-minus-skip policy;
- ADTS AAC packet-derived policy;
- Opus skip/discard policy;
- MPEG-TS packet PTS span policy;
- MPEGPS MP2 packet-count policy;
- WMAV2 sample-rate-aware packet-count-minus-decoder-delay policy;
- final `decodedSampleFramesTrust`, `decodedSampleFramesSource`, and `frameCountPolicyReason` decisions.

Behavior-change risk: high. This code protects Loading/Ready source-frame parity.

Validation command:

```bat
cmake --build build --config Release
```

Required comparisons:

- `decodedSampleFrames`
- `decodedSampleFramesKind`
- `decodedSampleFramesTrust`
- `decodedSampleFramesSource`
- `decodedSampleFramesBeforeCorrection`
- `gaplessCorrectionApplied`
- `skipSamplesStart`
- `skipSamplesEnd`
- `packetPtsSpanFrames`
- `packetDurationSumFrames`
- `packetFrameCountCandidateUsed`
- `frameCountPolicyReason`

Rollback strategy: keep the extracted function signature narrow and value-based so the old policy block can be restored without touching DLL exports.

## Stage D: Extract ProbeJsonWriter

Likely files changed:

- `src/Probe/ProbeJsonWriter.hpp/.cpp`
- `src/Utils/JsonUtils.hpp/.cpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Move:

- JSON string escaping;
- stream summary writing;
- fast probe JSON writing.

Behavior-change risk: medium. JSON field names and numeric formatting are externally consumed by AveVoice.

Validation command:

```bat
cmake --build build --config Release
```

Compare probe JSON text semantically. Field ordering may remain stable if that helps review, but field values are the compatibility requirement.

Rollback strategy: restore old writer helper block.

## Stage E: Extract Decode And Resample Service

Likely files changed:

- `src/Decode/AudioDecodeService.hpp/.cpp`
- `src/Decode/AudioResampler.hpp/.cpp`
- `src/Decode/PcmFormat.hpp/.cpp`
- `src/Ffmpeg/FfmpegDeleters.hpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Move:

- decoder open/send/receive;
- frame layout copy;
- `SwrContext` setup;
- converted interleaved float32 output;
- decoder/resampler flush.

Behavior-change risk: high. This touches final decoded PCM frames and sample conversion.

Validation command:

```bat
cmake --build build --config Release
```

Run import smoke on WAV, MP3, AAC, Opus, and WMA. Compare final `audio_info.json` frames and `original_f32.bin` byte size.

Rollback strategy: do not combine this stage with probe-policy movement.

## Stage F: Extract Streaming Import Service

Likely files changed:

- `src/Import/StreamingImportService.hpp/.cpp`
- `src/Import/WaveformChunkEmitter.hpp/.cpp`
- `src/Import/ProgressReporter.hpp/.cpp`
- `src/Utils/PathUtils.hpp/.cpp`
- `src/Dll/AveMediaBridgeDll.cpp`
- `CMakeLists.txt`

Move:

- streaming artifact lifecycle;
- `original_f32.bin` append/flush;
- progress and cancel polling;
- draft waveform chunk emission;
- temp JSON creation and final commit;
- failure/cancel cleanup.

Behavior-change risk: high. This is the path AveVoice uses for progressive import.

Validation command:

```bat
cmake --build build --config Release
```

Run progress, live-readable, waveform, and cancel smoke commands through `AveMediaBridgeLabApp`.

Rollback strategy: keep DLL exported functions routing through one new service object; if validation fails, route back to the old local implementation.

## Stage G: Shrink AveMediaBridgeDll.cpp

Likely files changed:

- `src/Dll/AveMediaBridgeDll.cpp`
- service headers already introduced by prior stages

Move:

- all remaining private implementation details out of the exported adapter;
- leave only argument validation, `try`/`catch`, return-code conversion, last-error text, and service calls.

Behavior-change risk: medium if earlier stages are already validated.

Validation command:

```bat
cmake --build build --config Release
```

Rollback strategy: because behavior already lives in extracted services, rollback should be limited to adapter wiring.

## Stage H: Regression Tests For Frame Count Policy

Likely files changed:

- test or lab tooling files only;
- optionally a small unit-style executable if the project adds one later.

Add/keep tests that lock:

- bare MP3 two-sided gapless correction;
- bare MP3 one-sided start skip only for `formatName == "mp3"` and codec MP3;
- MP3-in-MP4 packet-count-minus-skip policy;
- ADTS AAC packet-derived frame count;
- Opus gapless skip/discard frame count;
- MPEG-TS/M2TS/MTS AAC packet PTS span;
- MPEGPS MP2 packet count;
- WMAV2 sample-rate-aware packet frame length;
- unsafe CAF/ALAC remains non-authoritative when no exact candidate exists.

Behavior-change risk: low if tests are additive.

Validation command:

```bat
cmake --build build --config Release
```

Rollback strategy: remove only the failing test additions, not production policy.


# AveMediaBridge Architecture

## Goal

AveMediaBridge is a controlled FFmpeg-backed media bridge for the future AveVoice runtime. Its job is to accept user-selected audio/video files, expose stable media/audio metadata, decode audio into project-owned artifacts, and provide small smoke/convenience transforms for validation.

AveMediaBridge is not a FFmpeg clone, DAW, media player, GUI, RVC implementation, effects engine, or universal transcoder.

## Relation To AveVoice

AveVoice already owns its bootstrap/runtime folders:

```text
%APPDATA%\AveVoice\
  Settings\
  Presets\

%LOCALAPPDATA%\AveVoice\
  Logs\
  Temp\
  Session\
    Current\
```

AveVoice also owns `session.json`. Current session files reference the user's original media path and do not copy the source file into AppData:

```json
{
  "sourceMode": "Reference",
  "originalFileCopied": false,
  "mediaImported": false
}
```

AveMediaBridge is prepared to write decoded media artifacts for the current AveVoice session into:

```text
%LOCALAPPDATA%\AveVoice\Session\Current\Media\
```

The future AveVoice flow is:

```text
user drops one audio/video file
-> AveVoice stores sourcePath as a reference in session.json
-> AveMediaBridge probes/imports the referenced file
-> AveMediaBridge writes Media\metadata.json, Media\audio_info.json, Media\original_f32.bin
-> future waveform/effects/RVC pipeline consumes those artifacts
-> future transform writes a processed copy
```

AveMediaBridge is not directly connected to AveVoice yet.

## Runtime Layout

The future AveVoice release layout should be:

```text
Release\
  AveVoice.exe
  Modules\
    AveMediaBridge.dll
  Lib\
    ffmpeg\
      avcodec-61.dll
      avformat-61.dll
      avutil-59.dll
      swresample-5.dll
```

AveVoice's `ModuleLoader` must add the private FFmpeg folder before loading the bridge module:

```text
AddDllDirectory(Release\Lib\ffmpeg)
LoadLibraryW(Release\Modules\AveMediaBridge.dll)
GetProcAddress(...)
```

The LabApp build output also stages the FFmpeg runtime DLLs under `build\Release\Lib\ffmpeg\` so smoke tests exercise the same private dependency layout.

## CMake Targets

CMake builds three targets:

- `AveMediaBridgeCore`: static library containing the project-owned core API, FFmpeg-backed import/decode, WAV export, audio buffer helpers, stats, and processing helpers.
- `AveMediaBridge`: DLL module exposing the C ABI used by future AveVoice `ModuleLoader` calls.
- `AveMediaBridgeLabApp`: console lab application for manual import tests, the existing interactive menu, and DLL smoke commands.

## Public Boundary

Public project-owned C++ types are allowed inside the core static-library boundary. These include:

- `AudioBufferF32`
- `AudioStats`
- `SourceMediaInfo`
- `StreamSummary`
- `SelectedAudioStreamInfo`
- `DecodeReport`
- `MediaProbeDetails`
- `AudioImportResult`
- `FfmpegAudioImporter`
- `FfmpegWavAudioExporter`
- `AveMediaBridge`
- `AudioBufferProcessor`

The DLL boundary is C ABI only. It does not pass C++ classes, `std::string`, `std::vector`, `AudioBufferF32`, or ownership-sensitive allocations across module boundaries.

FFmpeg types are forbidden in public project headers and in the DLL ABI:

```text
AVFormatContext
AVCodecContext
AVPacket
AVFrame
SwrContext
AVChannelLayout
```

## Private FFmpeg Backend

FFmpeg is a private implementation detail under `src\Import` and `src\Export`.

Current import/decode uses FFmpeg libraries to:

- open media containers;
- discover streams;
- select the best audio stream;
- open the matching decoder;
- decode audio packets;
- resample/convert decoded samples to interleaved float32;
- fill `AudioImportResult`, metadata, decode report, warnings, and stats.

Current WAV export uses FFmpeg libraries to write PCM16 WAV files from `AudioBufferF32`.

No FFmpeg handle, struct, enum, or allocation contract is exposed to AveVoice.

## DLL API v1

All DLL functions use C ABI, return `0` on success, and return a non-zero code on failure. Functions catch internal exceptions and do not throw across the DLL boundary. Detailed failure text is available through `AveMediaBridge_GetLastErrorText`.

### AveMediaBridge_GetVersionString

```cpp
extern "C" __declspec(dllexport)
int AveMediaBridge_GetVersionString(
    wchar_t* outBuffer,
    int outBufferChars);
```

Purpose:

- copy the AveMediaBridge version/API string into a caller-owned UTF-16 buffer.

Inputs:

- `outBuffer`: caller-owned buffer.
- `outBufferChars`: buffer capacity in `wchar_t` characters including space for the null terminator.

Outputs:

- null-terminated version string on success.

Return code:

- `0`: success.
- non-zero: invalid buffer, buffer too small, or unexpected internal failure.

### AveMediaBridge_GetLastErrorText

```cpp
extern "C" __declspec(dllexport)
int AveMediaBridge_GetLastErrorText(
    wchar_t* outBuffer,
    int outBufferChars);
```

Purpose:

- copy the last DLL error text for the current thread into a caller-owned UTF-16 buffer.

Inputs:

- `outBuffer`: caller-owned buffer.
- `outBufferChars`: buffer capacity in `wchar_t` characters including space for the null terminator.

Outputs:

- null-terminated last error text, or an empty string if the current thread has no stored error.

Return code:

- `0`: success.
- non-zero: invalid buffer, buffer too small, or unexpected internal failure.

### AveMediaBridge_ProbeToJson

```cpp
extern "C" __declspec(dllexport)
int AveMediaBridge_ProbeToJson(
    const wchar_t* inputPath,
    const wchar_t* outputJsonPath);
```

Purpose:

- write media/audio probe information to a JSON file for AveVoice and smoke tooling.

Inputs:

- `inputPath`: source audio/video file path.
- `outputJsonPath`: destination JSON file path.

Outputs:

- JSON file containing source path, audio availability, selected audio stream index, sample rate, channel count, decoded frame count if known or estimated, duration, container/format fields, codec fields, stream summaries, warnings, and errors.

Return code:

- `0`: JSON was written successfully.
- non-zero: invalid input/output path, failed write, unavailable probe data for invalid media, or unexpected internal failure.

Current behavior:

- Probe uses Fast Probe v2, a bounded FFmpeg metadata path that does not full-decode audio.

Generated file:

```text
<outputJsonPath>
```

## Fast Probe v2

`AveMediaBridge_ProbeToJson` now uses `avformat_open_input`, bounded probe/analyze settings, `avformat_find_stream_info`, and `av_find_best_stream` to collect metadata quickly.

Fast Probe v2 rules:

- it does not run a full decode loop for basic metadata;
- it does not allocate `AudioBufferF32`;
- it does not create `original_f32.bin`;
- `decodedSampleFrames` can be `exact`, `estimated`, or `unknown`;
- `exact` is used only when decoded sample frames can be derived reliably without decode, such as PCM audio with stream duration in the sample-rate time base;
- compressed or container-estimated durations normally produce `estimated` or `unknown` frame counts;
- exact decoded frame counts are written by `AveMediaBridge_ImportAudioToSession` after the real import/decode path completes.

The v2 JSON keeps the v1 fields additively and adds:

- `schemaVersion`;
- `probeMode`;
- `bestAudioStreamIndex`;
- `containerFormat`;
- `channelLayout`;
- `durationKind`;
- `durationEstimationMethod`;
- `decodedSampleFrames`;
- `decodedSampleFramesKind`;
- `estimatedDecodedBytes`;
- `estimatedDecodedBytesKind`;
- `probeScore`.

### AveMediaBridge_ImportAudioToSession

```cpp
extern "C" __declspec(dllexport)
int AveMediaBridge_ImportAudioToSession(
    const wchar_t* inputPath,
    const wchar_t* sessionMediaDir);
```

Purpose:

- decode a referenced source media file and write AveVoice current-session audio artifacts.

Inputs:

- `inputPath`: source audio/video file path.
- `sessionMediaDir`: session media directory, normally `%LOCALAPPDATA%\AveVoice\Session\Current\Media\`.

Outputs:

```text
sessionMediaDir\
  metadata.json
  audio_info.json
  original_f32.bin
```

Return code:

- `0`: artifacts were written successfully.
- non-zero: invalid path, failed decode/import, failed artifact write, or unexpected internal failure.

Compatibility:

- `AveMediaBridge_ImportAudioToSession` remains the stable AveVoice v1 entrypoint.
- Internally it calls `AveMediaBridge_ImportAudioToSessionEx` with no progress or cancel callbacks.

### AveMediaBridge_ImportAudioToSessionEx

```cpp
struct AveMediaBridgeImportProgress
{
    uint32_t structSize;
    uint64_t framesWritten;
    uint64_t bytesWritten;
    uint64_t estimatedTotalFrames;
    uint64_t estimatedTotalBytes;
    double availableEndSec;
    double progress01;
    int sampleRate;
    int channels;
    uint32_t flags;
};

typedef void (__stdcall *AveMediaBridgeProgressCallback)(
    const AveMediaBridgeImportProgress* progress,
    void* userData);

typedef int (__stdcall *AveMediaBridgeCancelCallback)(
    void* userData);

struct AveMediaBridgeWaveformChunk
{
    uint32_t structSize;
    uint64_t firstFrame;
    uint32_t framesPerBin;
    uint32_t binCount;
    uint32_t valuesPerBin;
    int sampleRate;
    int channels;
    const float* minMaxPairs;
};

typedef void (__stdcall *AveMediaBridgeWaveformChunkCallback)(
    const AveMediaBridgeWaveformChunk* chunk,
    void* userData);

struct AveMediaBridgeImportOptions
{
    uint32_t structSize;
    const wchar_t* inputPath;
    const wchar_t* sessionMediaDir;
    AveMediaBridgeProgressCallback onProgress;
    AveMediaBridgeCancelCallback shouldCancel;
    void* userData;
    AveMediaBridgeWaveformChunkCallback onWaveformChunk;
};

extern "C" __declspec(dllexport)
int AveMediaBridge_ImportAudioToSessionEx(
    const AveMediaBridgeImportOptions* options);
```

Purpose:

- stream import audio to the existing session artifact contract while optionally reporting progress and accepting cancellation.

Return code:

- `0`: artifacts were written successfully.
- `4`: import was canceled by `shouldCancel`.
- other non-zero: invalid options, failed decode/import, failed artifact write, or unexpected internal failure.

## Current Import Memory Behavior

There are now two import/decode paths:

- LabApp manual import, processor tests, and `AveMediaBridge_TransformToWav` still use the core `AveMediaBridge::importAudio` path, which creates a full `AudioBufferF32` in memory because those workflows need in-memory sample processing.
- `AveMediaBridge_ImportAudioToSession` uses the streaming session import path and no longer allocates a full decoded `AudioBufferF32`.

## Streaming Import To Session

`AveMediaBridge_ImportAudioToSession` decodes the selected best audio stream incrementally and writes converted samples directly to the live `original_f32.bin` artifact.

Streaming session import rules:

- open the source with FFmpeg and select the best audio stream;
- decode packets/frames progressively;
- convert decoded chunks to interleaved little-endian float32 with a bounded resampler/output buffer;
- append each converted chunk directly to `original_f32.bin`;
- keep RAM usage bounded so it does not scale with media duration;
- flush successful chunk writes before publishing exact `framesWritten` and byte progress;
- verify `bytesWritten == framesWritten * channels * sizeof(float)`;
- write `audio_info.json.tmp` and `metadata.json.tmp`;
- commit `metadata.json` and then `audio_info.json` only after successful decode and validation;
- delete the partial `original_f32.bin` plus tmp json files on cancel/failure so a failed import does not leave valid-looking partial artifacts.

The final artifact names and schemas remain compatible with the existing AveVoice contract. Exact decoded frames are written in `audio_info.json` after import completes.

## Live Readable PCM During Streaming Import

During streaming session import, `original_f32.bin` is the growing readable PCM artifact. It is created before any progress callback with `framesWritten > 0`, and `framesWritten`/`bytesWritten` are published only after the corresponding float32 bytes have been written and flushed to that file.

`audio_info.json` and `metadata.json` remain the final commit gate. While import is running, their `.tmp` forms may exist, but the final `audio_info.json` is not committed until the decoded byte count has been validated. This prevents AveVoice from seeing a valid-looking final artifact pair before import succeeds.

On cancel or failure, AveMediaBridge closes the writer, deletes the partial `original_f32.bin`, deletes `audio_info.json.tmp` and `metadata.json.tmp`, and returns the appropriate error or cancel code. This supports AveVoice partial playback during progressive import: the committed range from progress callbacks never intentionally advances beyond bytes that are physically available in `original_f32.bin`.

The LabApp long-file smoke tools print throttled console progress instead of logging every callback. Progress lines include percent when known, current/estimated time, frames written, MB written, waveform chunk count where applicable, simple MB/s speed, and a final summary.

## Disk Preflight

Before streaming decode starts, session import estimates decoded float32 bytes from available duration, sample rate, and channel metadata. If the estimate is known and free space in `sessionMediaDir` is clearly below the estimated decoded byte count plus a small headroom, import fails before decode.

If the decoded-size estimate is unknown, import continues and records a warning in `metadata.json` because exact bytes will be known only while streaming samples to disk.

## Import Progress And Cancellation API

`AveMediaBridge_ImportAudioToSessionEx` is a C ABI, callback-based extension for streaming session import. It does not change artifact names, artifact schemas, or the old `AveMediaBridge_ImportAudioToSession` ABI.

Progress and cancellation rules:

- callbacks are invoked synchronously on the same caller/import thread;
- the DLL does not create hidden worker threads;
- `onProgress` receives `AveMediaBridgeImportProgress` snapshots with monotonic `framesWritten`;
- `progress01` is in the `0..1` range when an estimate is known, otherwise `-1.0`;
- `availableEndSec` is `framesWritten / sampleRate` when sample rate is known;
- progress callbacks are throttled and also emitted at final completion;
- `shouldCancel` is polled during the decode loop;
- cancellation stops decode cleanly, closes files, deletes the partial `original_f32.bin` and temporary json artifacts, returns cancel code `4`, and sets last error text to a clear canceled message.

The API keeps RAM bounded because progress is driven from the streaming import counters rather than from a full decoded `AudioBufferF32`. It is also the foundation for future AveVoice progressive waveform UX.

## Draft Waveform Chunk Callback

`AveMediaBridge_ImportAudioToSessionEx` can optionally emit draft waveform chunks during streaming import through `AveMediaBridgeImportOptions::onWaveformChunk`.

The callback is additive and optional:

- callers compiled against the older progress/cancel options layout continue to work because `structSize` is respected;
- if `onWaveformChunk` is null or the caller passed the older struct size, import behavior is unchanged;
- the old `AveMediaBridge_ImportAudioToSession` entrypoint remains compatible.

Draft waveform chunk rules:

- callbacks are invoked synchronously on the same import thread;
- the DLL does not create hidden worker threads;
- chunks are generated from the already decoded and converted interleaved float32 samples used for `original_f32.bin`;
- samples are downmixed to mono by averaging channels;
- NaN and Inf samples are treated as zero for waveform purposes;
- each bin stores min/max as two float values, so `valuesPerBin == 2`;
- `framesPerBin` is fixed for the draft stream and currently uses 128 source frames per bin, matching AveVoice's final waveform pyramid base level;
- `firstFrame` is monotonic across chunks;
- `minMaxPairs` is valid only for the duration of the callback and must not be stored by the caller;
- memory remains bounded by the decoder/resampler buffers plus a small pending waveform chunk buffer;
- cancellation is still polled during import, and canceled imports still clean temporary artifacts.

These chunks are temporary UI-oriented data for future progressive waveform UX. They are not the canonical final waveform format, are not written to final waveform files inside AveMediaBridge, and do not replace AveVoice's separate final `WaveformPeaksBuilder` stage.

No C++ ownership crosses the DLL boundary: the callback receives a POD struct and a borrowed `const float*` valid only during the call. FFmpeg types remain private to the implementation.

### AveMediaBridge_TransformToWav

```cpp
extern "C" __declspec(dllexport)
int AveMediaBridge_TransformToWav(
    const wchar_t* inputPath,
    const wchar_t* outputWavPath,
    float gain);
```

Purpose:

- smoke/convenience transform that imports media audio, applies gain, and writes PCM16 WAV.

Inputs:

- `inputPath`: source audio/video file path.
- `outputWavPath`: destination WAV path.
- `gain`: linear gain applied to decoded float32 samples before export.

Outputs:

- PCM16 WAV at `outputWavPath`.

Return code:

- `0`: WAV was written successfully.
- non-zero: invalid path, failed import/decode, failed export, or unexpected internal failure.

### AveMediaBridge_ExportF32ToWav

Planned future API, not implemented in the current DLL:

```cpp
extern "C" __declspec(dllexport)
int AveMediaBridge_ExportF32ToWav(
    const wchar_t* audioF32Path,
    const wchar_t* audioInfoJsonPath,
    const wchar_t* outputWavPath);
```

Expected purpose:

- read an `original_f32.bin`-style float32 artifact plus its `audio_info.json`, then write a PCM16 WAV copy.

### Legacy Compatibility Export

`AveMediaBridge_GetVersion` is still exported as a legacy compatibility helper returning a static `const wchar_t*`. New callers should prefer `AveMediaBridge_GetVersionString` because it uses a caller-owned buffer.

## Artifact Contract

`AveMediaBridge_ImportAudioToSession` writes exactly these current v1 files:

```text
metadata.json
audio_info.json
original_f32.bin
```

### metadata.json

`metadata.json` contains:

- `apiVersion`;
- `sourcePath`;
- `hasAudio`;
- container/format name and long name;
- duration if available;
- stream count and audio stream count;
- selected audio stream index;
- codec name/id;
- selected stream details;
- decode report fields currently available;
- decoded audio shape;
- expected data byte count;
- audio stats;
- stream summaries;
- warnings;
- errors.

### audio_info.json

`audio_info.json` contains:

```json
{
  "sampleRate": 44100,
  "channels": 2,
  "frames": 44100,
  "sampleFormat": "float32",
  "sampleLayout": "interleaved",
  "dataFile": "original_f32.bin"
}
```

### original_f32.bin

`original_f32.bin` is raw audio sample data:

- sample format: IEEE 754 `float32`;
- byte order: little-endian;
- layout: interleaved;
- channel order: FFmpeg-decoded output channel order;
- no header;
- no metadata inside the binary file.

Expected size:

```text
frames * channels * sizeof(float)
```

For example, `44100` stereo frames require:

```text
44100 * 2 * 4 = 352800 bytes
```

## Current Session Import Flow

Current intended AveVoice-ready flow:

```text
AveVoice session.json sourcePath
-> AveMediaBridge_ProbeToJson(sourcePath, probe.json)
-> AveMediaBridge_ImportAudioToSession(sourcePath, Current\Session\Media)
-> metadata.json
-> audio_info.json
-> original_f32.bin
-> future waveform/effects/RVC
```

AveVoice remains responsible for UI state, user interaction, session ownership, effect/RVC settings, and processed-copy destination choices.

## TransformToWav Role

`AveMediaBridge_TransformToWav` is a smoke/convenience function. It proves that the DLL can be loaded, FFmpeg private dependencies can be resolved, media can be decoded, simple sample processing can run, and WAV output can be written.

It is not the main future AveVoice pipeline. The future pipeline should use probe/import artifacts and later dedicated transform/export APIs.

## LabApp

`AveMediaBridgeLabApp.exe` keeps the existing interactive menu:

```text
1. Import one media file
2. Import all files from folder
3. Run AudioBufferF32 processor tests on one file
4. Show FFmpeg library versions
5. Show project paths
0. Exit
```

It also supports CLI smoke commands:

```text
AveMediaBridgeLabApp.exe smoke-dll-transform "<input-file>" "<output-wav>"
AveMediaBridgeLabApp.exe smoke-dll-probe "<input-file>" "<output-json>"
AveMediaBridgeLabApp.exe smoke-dll-import-session "<input-file>" "<session-media-dir>"
AveMediaBridgeLabApp.exe smoke-dll-import-session-progress "<input-file>" "<session-media-dir>"
AveMediaBridgeLabApp.exe smoke-dll-import-session-live-readable "<input-file>" "<session-media-dir>"
AveMediaBridgeLabApp.exe smoke-dll-import-session-waveform "<input-file>" "<session-media-dir>"
AveMediaBridgeLabApp.exe smoke-dll-import-session-cancel "<input-file>" "<session-media-dir>"
```

The smoke commands dynamically load `AveMediaBridge.dll`, add `build\Release\Lib\ffmpeg\` to the DLL search path, resolve the target function with `GetProcAddress`, run the API call, and print DLL last-error text on failure.

## Current Scope

Implemented:

- FFmpeg-backed import/decode;
- `AudioBufferF32`;
- media/source/stream/decode metadata structs;
- audio stats;
- simple gain/normalization/mono helper processing inside core;
- WAV PCM16 export;
- DLL module;
- DLL smoke transform;
- `AveMediaBridge_GetVersionString`;
- thread-local `AveMediaBridge_GetLastErrorText`;
- `AveMediaBridge_ProbeToJson`;
- Fast Probe v2 no-decode metadata probing;
- streaming `AveMediaBridge_ImportAudioToSession`;
- `AveMediaBridge_ImportAudioToSessionEx` progress/cancel callbacks;
- draft waveform chunk callbacks during streaming import;
- disk preflight for session import;
- LabApp smoke commands for transform, probe, and session import.

Not implemented:

- GUI;
- RVC;
- waveform renderer;
- effects engine;
- video remux;
- compressed audio export;
- `AveMediaBridge_ExportF32ToWav`;
- selected-stream import API;
- direct AveVoice integration.

## Error Handling

DLL functions do not throw exceptions across the DLL boundary.

Rules:

- success returns `0`;
- failures return a non-zero code;
- human-readable failure details are stored in DLL-owned thread-local state;
- callers retrieve details with `AveMediaBridge_GetLastErrorText`;
- output buffers are caller-owned;
- complex results are written to JSON files and binary artifact files.

## Design Rules

- No FFmpeg types in public project headers.
- No FFmpeg types in the DLL ABI.
- No C++ classes across the DLL ABI.
- No `std::string`, `std::vector`, STL containers, or ownership-bearing C++ types across the DLL ABI.
- Use paths, files, JSON, and binary artifacts as the stable boundary.
- `AudioBufferF32` remains a project-owned C++ core boundary, not a DLL ABI boundary.
- `original_f32.bin` is the stable raw sample artifact for waveform/effects/RVC preparation.

## Future API

Possible future functions:

- `AveMediaBridge_BuildWaveformPeaks`;
- `AveMediaBridge_ExportF32ToWav`;
- `AveMediaBridge_ExportCompressedAudio`;
- `AveMediaBridge_RemuxVideoWithAudio`;
- import selected audio stream;
- richer fast probe for streams, chapters, tags, language, disposition, time bases, and exact durations;
- transform/export APIs that consume AveVoice session artifacts and produce processed copies.

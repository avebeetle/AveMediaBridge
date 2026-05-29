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

- JSON file containing source path, audio availability, selected audio stream index, sample rate, channel count, decoded frame count if known, duration, container/format fields, codec fields, stream summaries, warnings, and errors.

Return code:

- `0`: JSON was written successfully.
- non-zero: invalid input/output path, failed write, unavailable probe data for invalid media, or unexpected internal failure.

Current v1 behavior:

- Probe uses the existing FFmpeg-backed import/decode path. This means it can decode audio to learn frame count and output audio shape. A future fast probe can avoid full decode while preserving the JSON contract.

Generated file:

```text
<outputJsonPath>
```

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
- `AveMediaBridge_ImportAudioToSession`;
- LabApp smoke commands for transform, probe, and session import.

Not implemented:

- GUI;
- RVC;
- waveform renderer;
- effects engine;
- video remux;
- compressed audio export;
- `AveMediaBridge_ExportF32ToWav`;
- fast no-decode probe;
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

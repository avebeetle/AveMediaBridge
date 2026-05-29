# AveMediaBridge

AveMediaBridge is a controlled FFmpeg-backed media bridge for AveVoice.

It provides:
- FFmpeg-backed media/audio probe and import;
- decoded float32 interleaved audio artifacts;
- WAV PCM16 smoke transform/export;
- AveMediaBridgeCore static library;
- AveMediaBridge.dll C ABI module;
- AveMediaBridgeLabApp manual smoke tool.

AveMediaBridge is not:
- a GUI;
- a DAW;
- an RVC implementation;
- an effects engine;
- a universal transcoder.

## Current runtime role

AveVoice stores the user source file as a reference. AveMediaBridge decodes audio into session artifacts:

```text
Session\Current\Media\
  metadata.json
  audio_info.json
  original_f32.bin
```

## Local dependencies

FFmpeg is a local/private dependency under:

```text
ffmpeg/
```

This folder is not committed.

## Build

```bat
build_release.bat
```

or:

```bat
cmake --build build --config Release
```

## Git policy

Do not commit:
- build artifacts;
- FFmpeg binaries, headers, or libs;
- generated output files;
- media fixtures through normal Git;
- credentials or secrets.

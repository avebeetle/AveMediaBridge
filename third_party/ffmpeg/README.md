# FFmpeg 7.1.4 Matroska CodecDelay Backport

AveMediaBridge uses FFmpeg 7.1.4 Profile B with upstream commit
`0880458e4c27337718ca836e1193803a089fc690` applied to the Matroska
demuxer. The runtime keeps the FFmpeg 7.1 ABI and the existing component
allowlist.

```cpp
// Upstream Matroska demuxer backport: propagate CodecDelay as
// initial skip-sample authority for decoded presentation.
```

The full upstream format-patch is stored under `patches/`. The build applies
its production hunk to `libavformat/matroskadec.c`; newer-tree FATE reference
updates in the same commit are retained in the patch for provenance but are
not applicable to the 7.1.4 release test baselines.

`build_profile_b_matroska_codec_delay_backport.bat` accepts an official
`ffmpeg-7.1.4.tar.xz` and a laboratory output root. It verifies the source and
patch hashes, refuses to overwrite an existing candidate, applies the exact
upstream hunk, and builds the established MSVC x64 shared Profile B.

FFmpeg headers, import libraries, and DLLs remain local under `ffmpeg/` and
are intentionally ignored by Git. Their certified hashes and build inputs are
recorded in `build-manifest/ffmpeg-7.1.4-matroska-codec-delay-backport.json`.

See `docs/dependencies/FFmpeg714MatroskaCodecDelayBackport.md` for promotion
and regression commands.

## Short packet imports

Build the candidate from the same archive with
`tools/export/BuildExportRuntime.ps1 -Archive <ffmpeg-7.1.4.tar.xz> -LabRoot <lab> -OutputRoot <new-directory> -ShortPacketImports`.
This profile includes the Matroska CodecDelay backport, the single-page Vorbis
correction and the three hash-checked patches listed in the generated
`candidate-manifest.json`:

- Preserve the other end of an existing packet trim when FFmpeg injects an
  initial skip or terminal discard. A single Matroska Opus packet may need both.
- Use the MOV sample-size table to index variable-size ALAC packets when their
  duration is a single tick and the legacy fixed-byte chunk layout is unavailable.
- Recognize short runs of consistent complete elementary AAC or AC-3 family
  frames when a single false sync word otherwise ties the audio probe.

The compiler, component allowlist and DLL ABI follow the existing WAV Float32
export profile. `AveMediaBridgeTests.short_packet_imports` exercises the installed
demuxer through the public Bridge API and compares the complete output with
Ogg Opus controls and original lossless PCM.

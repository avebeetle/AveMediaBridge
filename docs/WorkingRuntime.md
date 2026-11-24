# Accepted runtime and development checkout

## Import boundaries and verified loading scales, 2026-11-24

The [five-file receipt](../tools/runtime/accepted-runtime-20261124-import-boundaries.json)
pins the Bridge built from `86940232f771d74662d6fc97d331117a3a7871fa` and the
FFmpeg 7.1.4 runtime. The new format DLL preserves both ends of a short Opus
packet, reads one-tick ALAC in MOV and recognizes complete short elementary
AAC and Dolby streams. Bridge checks weakly detected elementary audio for
decode errors. It measures qualified DVD VOB/AC-3 and FLV/MP3 loading scales
from the selected audio while decoding the stream through EOF.

The snapshot under `out/runtime-baselines/2026-11-24-import-boundaries/` contains
the exact Bridge Git archive and full corresponding FFmpeg sources for both builds.
The new `avformat-61.dll` uses the five-patch `-ShortPacketImports` recipe.
The original candidate-002 provides the codec, utility and resampling DLLs.
The receipt maps each library to its source archive and build manifest. The
snapshot includes the upstream Xiph archives, private patch, licenses and notices.

All 897 short cases produced the expected import results. For 124 medium
files the decoded PCM matched the saved hashes. The three new regression
suites also passed against the five files in the snapshot. The Release CTest
run passed 24 tests and skipped nine suites requiring external fixtures.

## Pinned CAF/ALAC sample counts, 2026-11-23

The optional CAF export summed all ALAC packets in the 40-minute stereo corpus
file and returned 105,840,000 frames per channel. The same pass established
the source SHA-256. Qualification covered short mono and stereo recordings,
short interior packets, six headers crossing input blocks and the ordinary
fallback for unsupported edit, trim and packet layouts. The inspector passed
40 cases. The importer matched the expected PCM across 29 short files.

The [five-file receipt](../tools/runtime/accepted-runtime-20261123-caf-alac.json)
records the new Bridge DLL with the same four FFmpeg DLLs. Its snapshot
contains the source archive, inspection and import results, export-name check
and Release CTest log. Twenty-one tests passed. Nine existing suites require
separate external fixtures.

## Video track inventory, 2026-11-23

The quick probe reports whether the bundled decoder found a video stream
without the attached-picture disposition. A Matroska MJPEG movie returned true.
An AAC file with an attached JPEG, an AAC file with subtitles and a file with
two audio streams all returned false.

The [five-file receipt](../tools/runtime/accepted-runtime-20261123-matroska-video-track.json)
records the Bridge and the same four FFmpeg DLLs. Its snapshot includes the
source archive, Release test log and comparison of all 73 corpus probes.
The previous JSON fields matched apart from elapsed scan times. Twenty tests
passed and nine required unavailable external fixtures.

## Matroska AAC import with a longer picture track, 2026-11-23

The previous Bridge rejected a short stereo AAC track when the movie duration
implied approximately 97 GB of decoded audio on a volume with less space
available. The updated Bridge opens the same audio even when the pictures span
that interval. The decoded PCM matched the reference byte for byte. An
audio-only Matroska control retained its disk-space check.

The [five-file receipt](../tools/runtime/accepted-runtime-20261123-matroska-aac-disk.json)
records the Bridge and the four previously qualified FFmpeg DLLs. The snapshot
under `out/runtime-baselines/2026-11-23-matroska-aac-disk/` includes the source
archive, the original refusal, the Release CTest report and the probe comparison
for 73 files. Twenty tests passed. Nine tests require separate external fixtures.

## Selected Vorbis audio in Matroska and WebM video, 2026-11-23

The bounded probe measures native Vorbis packet overlap for the initial waveform
width in small mono/stereo videos. Six stereo fixtures with short and long video
tracks produced the same respective audio extents of 44,736, 882,368 and 2,646,720
frames. The writer reads all PCM through EOF. A selected-track duration tag may
provide progress for larger files. Testing included a file with 44,608 audio
frames whose tag implied more than 120 GB of output; both import APIs wrote
the actual samples.

The [five-file receipt](../tools/runtime/accepted-runtime-20261123-video-audio-extent.json)
identifies the qualified Bridge DLL and the four previously qualified FFmpeg
DLLs. The staging snapshot at `out/runtime-baselines/2026-11-23-video-audio-extent/`
includes the exact source archive, six probe comparisons and the Release CTest
log. Twenty tests passed. Nine historical tests skipped unavailable external
fixtures. The previous runtime snapshot is preserved.

## Extended Ogg Vorbis length verification, 2026-11-23

When the bounded check of an audio-only Vorbis file exhausts its budget, AveVoice
can request a cancellable sequential check on a pinned source. The prepared
reader then imports from that same source. At qualification the files with a
large comment and with 32 chained streams reached Ready, with all 48,011 and
193,538,144 frames respectively compared to Xiph. Corrupt tails, unsupported
channel changes, and cancellation were also checked.

The [runtime receipt](../tools/runtime/accepted-runtime-20261123-vorbis-extended.json)
records the new Bridge DLL and the four FFmpeg DLLs from the previous runtime.
`out/runtime-baselines/2026-11-23-vorbis-extended/` contains the verified five
files, a Git archive of the exact Bridge source commit, and qualification results.
The full CTest run recorded 18 passes and 9 skipped corpus-dependent tests.

## Chained Ogg Vorbis, 2026-11-23

The [runtime receipt](../tools/runtime/accepted-runtime-20261123-vorbis-links.json) pins
the new Bridge and the four previously shipped FFmpeg DLLs. The Bridge includes
the private Xiph code described in the [build instructions](../third_party/xiph/README.md).
The archived package, source and qualification results are under
`out/runtime-baselines/2026-11-23-vorbis-links/`. The short-Vorbis runtime below is retained.

The selected audio-only Ogg path verifies logical stream boundaries before import.
On the short mixed-rate regression it imports all 48,044 document frames. Two guarded
imports exceeding 2 GiB matched the qualified full PCM hashes. Native single-link
six-channel audio is supported, including very short files. A switch from stereo to
5.1 during a chain produces an explicit refusal. Files that exhaust the quick probe
also receive a distinct refusal. This includes a previously importable single-link
file with a 1.4 MB comment. No partial document is published in that case.

## Short Vorbis streams, 2026-11-22

The [five-file receipt](../tools/runtime/accepted-runtime-20261122-vorbis.json) identifies the Product runtime for short Ogg, OGA and OGV files. It uses the previously qualified Bridge, codec, resampler and utilities, plus a full MSVC build of the Ogg demuxer. The demuxer accounts for initial Vorbis overlap when the first audio packet is on the final audio page, including packets spanning multiple lacing segments. Exact packet boundaries keep the terminal duration and trim consistent with the decoded signal.

The exact set is archived under `out/runtime-baselines/2026-11-22-short-vorbis/` with qualification evidence. Rebuild FFmpeg with `tools/export/BuildExportRuntime.ps1 -Archive <ffmpeg-7.1.4.tar.xz> -LabRoot <lab> -OutputRoot <new-directory-under-lab> -ShortVorbis`. The build manifest records the original archive, the Matroska and Vorbis patches, compiler and configuration. Packaging selects the new `avformat-61.dll` and verifies the other four runtime files against the previous receipt. The earlier MKV snapshot is retained. The CTest `short_vorbis_import` generates single-page and multi-page versions of identical compressed audio, then checks every PCM frame at five lengths in each container.

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

For a build against the short-Vorbis runtime, use the headers and import libraries under the completed FFmpeg build's `install/`. Populate the SDK's `bin/` with the four FFmpeg DLLs from the accepted snapshot when qualifying the exact shipped set. Configure a fresh Bridge build with that SDK from the repository root:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DAVEMEDIABRIDGE_FFMPEG_ROOT="<qualified-ffmpeg-sdk>"
cmake --build build --config Release
cmake -E env AMBE_EXPECT_FLOAT_WAV=1 ctest --test-dir build -C Release -j1 --output-on-failure
```

The former CMake cache is preserved with the full backups. Generated files containing the old absolute source path are not reused. The optional reader laboratory roots remain empty because that historical corpus is unavailable; available container and reader-media-facts fixtures can still be configured independently. None of these commands invokes AveVoice deployment.

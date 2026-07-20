# FFmpeg 7.1.4 Matroska CodecDelay Backport

## Scope

AveMediaBridge retains FFmpeg 7.1.4 and its `61/61/59/5` DLL ABI. Upstream
commit `0880458e4c27337718ca836e1193803a089fc690` makes the Matroska
demuxer propagate an audio track's already parsed `CodecDelay` as initial
skip-sample authority.

This removes leading codec priming. It does not trim terminal audio, change
AAC decoding, or add container/codec policy to AveMediaBridge.

## Reproducible Build

Download the official `ffmpeg-7.1.4.tar.xz` and verify SHA-256
`71F4AAC3573ED9060489CB62526A6C7DDA815AE10993789611ACD7BE9FA9FBF4`.
Then run:

```bat
third_party\ffmpeg\build_profile_b_matroska_codec_delay_backport.bat ^
  D:\downloads\ffmpeg-7.1.4.tar.xz ^
  D:\rvc\c++\DragonianVoice\ffmpeglab
```

The script verifies the exact upstream patch, creates isolated source,
build, and install directories, and refuses to overwrite an existing
candidate.

## Bridge Candidate

Configure without replacing the local stable runtime:

```bat
cmake -S . -B build-candidate ^
  -DAVEMEDIABRIDGE_FFMPEG_ROOT=D:\rvc\c++\DragonianVoice\ffmpeglab\install\profile_b_ffmpeg_7_1_4_matroska_codec_delay_backport
cmake --build build-candidate --config Release
ctest --test-dir build-candidate -C Release --output-on-failure
```

The Matroska test accepts external media because binary media fixtures are
not committed. Its default paths target the sibling 40-minute corpus. A
small deterministic fixture can be generated outside the repository with:

```powershell
tests\media\generate_matroska_codec_delay_fixture.ps1 `
  -FfmpegExe D:\path\to\ffmpeg.exe `
  -OutputDirectory D:\temp\matroska-codec-delay-fixture
```

For a custom fixture, configure the media/master paths plus
`AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_EXPECTED_FRAMES` and
`AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_EXPECTED_SKIP`.

## Promotion

The local `ffmpeg/` directory is ignored by Git. Promotion copies the
certified candidate's `include`, `lib`, `bin`, and license files into that
existing local dependency layout only after direct PCM, Bridge corpus, and
AveVoice integration certification. The hashes in the build manifest are
the deployment gate.

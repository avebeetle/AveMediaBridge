# Ogg/Vorbis PCM Extent Authority Audit

## 1. User-visible downstream symptom

AveVoice previously showed a progressive Loading timeline ending at
`105840640` frames for
`06_ogg_vorbis_stereo_44100.ogg`. After decode completed, final session audio
reported `105840000` frames, so the viewport reconciled by `-640` frames
(`-0.014512471655` seconds at 44100 Hz).

This audit separates the AveMediaBridge output from AveVoice's downstream
provisional waveform sizing. On current AveMediaBridge HEAD
`44cc0fa5affeac773697fdf2b020afe9c73e38cb`, AveMediaBridge itself publishes
`105840000`, not `105840640`.

## 2. Exact media identity

- Path:
  `D:\rvc\c++\DragonianVoice\AveMediaBridge_tests_40min_speech\06_ogg_vorbis_stereo_44100.ogg`
- Size: `35339801` bytes
- Container: `ogg`
- Codec: `vorbis`
- Sample rate: `44100`
- Channels: `2`

## 3. Initial 105840640 origin

The `105840640` value is not produced by AveMediaBridge at this HEAD. The
current DLL probe JSON contains:

- `frames = 105840000`
- `decodedSampleFrames = 105840000`
- `decodedSampleFramesKind = estimated`
- `decodedSampleFramesTrust = unsafe_estimated`
- `decodedSampleFramesSource = stream_duration_estimate`

Read-only consumer correlation shows the downstream origin:

- File:
  `D:\rvc\c++\DragonianVoice\AveVoice\src\App\Actions\WaveformPeakSourceHelpers.cpp`
- Function: `readProgressiveViewportProbeEstimate`
- Branch: `applyFrameEstimate(decodedFrames, frameCountIsAuthoritative(), preserveExactAuthoritativeFrameCount())`
- Rule for non-preserved/non-authoritative counts:
  `ceilDivide(frames, 1024) * 1024`

For this fixture:

```text
AveMediaBridge decodedSampleFrames = 105840000
105840000 / 1024 = 103359.375
ceil(...) = 103360
103360 * 1024 = 105840640
```

So the observed `105840640 -> 105840000` shrink is a boundary consequence:
AveMediaBridge marks an exact-looking Ogg/Vorbis extent as provisional, and
AveVoice pads provisional extents to its 1024-frame pyramid grid.

## 4. Final 105840000 origin

The exact final count comes from the normal decode/resample/write accounting:

- `src/Dll/AveMediaBridgeDll.cpp::receiveStreamingFrames` receives decoded
  `AVFrame`s.
- `writeConvertedStreamingSamples` calls `swr_convert`.
- `addWrittenFrames` increments `framesWritten` from converted output frames.
- `writeStreamingAudioInfoJson` writes final `audio_info.json.frames`.

Audit result:

```text
decoderRawSampleSum = 105840000
decoderEffectiveSampleSum = 105840000
resamplerInputFrames = 105840000
resamplerOutputFrames = 105840000
resamplerFlushFrames = 0
writerFrames = 105840000
writerBytes = 846720000
writerFrameRemainderBytes = 0
```

## 5. FFmpeg linked/runtime versions

Linked DLL versions used by the audit executable:

```text
libavformat compile/runtime = 61.7.102 / 61.7.102
libavcodec  compile/runtime = 61.19.101 / 61.19.101
libavutil   compile/runtime = 59.39.100 / 59.39.100
libswresample compile/runtime = 5.3.100 / 5.3.100
```

CLI reference:

```text
ffmpeg 7.1.1 essentials
libavformat = 61.7.100
libavcodec = 61.19.101
libavutil = 59.39.100
libswresample = 5.3.100
```

The CLI and linked libraries agree on the exact media extent for this file, but
`libavformat` micro versions differ, so CLI is supporting evidence rather than
the primary authority.

## 6. Format duration

Bounded and full FFmpeg metadata both report:

```text
AVFormatContext::duration = 2400000000 us
format duration seconds = 2400.000000
format duration frames floor/nearest/ceil = 105840000 / 105840000 / 105840000
production llround(durationSec * sampleRate) = 105840000
```

No format-duration formula yields `105840640` on this HEAD with this fixture.

## 7. Stream duration

Bounded and full FFmpeg metadata both report:

```text
AVStream::duration = 105840000
AVStream::time_base = 1/44100
stream duration frames floor/nearest/ceil = 105840000 / 105840000 / 105840000
production llround(streamSeconds * sampleRate) = 105840000
```

`src/Probe/MediaProbeService.cpp::estimateFastDurationAndFrames` uses
`secondsFromStreamDuration(audioStream)` first, then computes:

```text
decodedSampleFrames = llround(document.durationSec * sampleRate)
```

For this fixture that formula is:

```text
llround(2400.0 * 44100) = 105840000
```

## 8. Rational frame candidates

The audit executable records integer/rational candidates with FFmpeg rescale
functions:

```text
formatDurationFramesFloor   = 105840000
formatDurationFramesNearest = 105840000
formatDurationFramesCeil    = 105840000

streamDurationFramesFloor   = 105840000
streamDurationFramesNearest = 105840000
streamDurationFramesCeil    = 105840000
```

There is no duration-to-frame rounding error for the Ogg/Vorbis fixture.

## 9. Ogg granule timeline

The audit-only Ogg page reader reports:

```text
lastGranulePosition = 105840000
lastEosGranulePosition = 105840000
serialNumberCount = 1
chained = false
truncated = false
```

FFmpeg packet timing agrees:

```text
first audio packet pts = -128, duration = 128
first decoded frame pts = 0, nb_samples = 576
last audio packet pts = 105839872, duration = 128
last decoded frame pts = 105839872, nb_samples = 128
```

The Ogg/Vorbis final granule is the exact decoded PCM end for this file.

## 10. Skip/discard and codec padding

Audit results:

```text
skipSamplesAtStart = 0
discardPaddingAtEnd = 0
codecDelay = 0
initialPadding = 0
trailingPadding = 0
```

The `640`-frame downstream shrink is not codec delay, initial padding, trailing
padding, packet skip/discard side data, or Vorbis granule trimming.

## 11. Decoder accounting

The decoder emits:

```text
decodedFrameObjectCount = 206388
sumDecoderNbSamples = 105840000
firstFramePts = 0
firstFrameNbSamples = 576
lastFramePts = 105839872
lastFrameNbSamples = 128
```

The decoder output count is already exact and equals the Ogg EOS granule.

## 12. Resampler accounting

The source and output sample rate are both `44100`. Swresample is still used to
convert Vorbis planar float (`fltp`) to interleaved float (`flt`), but it does
not change the frame count:

```text
resamplerInputFrames = 105840000
resamplerOutputFrames = 105840000
resamplerFlushFrames = 0
```

## 13. Writer accounting

The writer-equivalent counter uses stereo float32:

```text
bytesPerFrame = 2 * 4 = 8
writerBytes = 846720000
writerFrames = 846720000 / 8 = 105840000
writerFrameRemainderBytes = 0
```

There is no writer accounting loss.

## 14. Progress/public API semantics

AveMediaBridge's progressive progress denominator comes from:

- `src/Dll/AveMediaBridgeDll.cpp::runStreamingSessionImportToLiveFile`
- `estimateDecodedBytesForPreflight`
- `Probe::estimateDecodedBytesForPreflight`
- `src/Probe/MediaProbeService.cpp::estimateFastDurationAndFrames`
- `emitImportProgress`, which assigns
  `progress.estimatedTotalFrames = result.preflightEstimatedFrames`

For this Ogg/Vorbis fixture the current public value is `105840000`, but the
trust is `unsafe_estimated`. The current API already distinguishes estimate from
authoritative with `decodedSampleFramesTrust`, `decodedSampleFramesKind`, and
`decodedSampleFramesSource`.

AveVoice receives that distinction, treats the frame count as provisional, and
uses its own 1024-aligned Loading extent.

## 15. First moment exact count is knowable

For this exact fixture:

```text
exactExtentAvailableAtProbe = yes
exactExtentAvailableBeforeInteractiveLoading = yes
exactExtentAvailableOnlyAfterDecode = no
```

The evidence is available before interactive Loading:

- bounded FFmpeg stream duration is already `105840000` in time base `1/44100`;
- Ogg EOS granule is `105840000`;
- full decode and writer accounting later confirm the same count.

Current AveMediaBridge policy does not promote Ogg/Vorbis stream/granule
evidence to authoritative.

## 16. Cross-format controls

Existing external 40-minute controls were run with the same audit executable:

```text
WAV PCM16 44100: initial=105840000, trust=authoritative, final=105840000
MP3 44100: initial=105840000, trust=authoritative, final=105840000
FLAC 44100: initial=105840000, trust=unsafe_estimated, final=105840000
ADTS AAC 44100: initial=105841664, trust=authoritative, final=105841664
M4A AAC 44100: initial=105840000, trust=unsafe_estimated, final=105840640
Ogg Opus 48000: initial=115200000, trust=authoritative, final=115200000
```

The Ogg/Vorbis `640` shrink is not systemic decoder loss. The broader systemic
pattern is conservative trust classification for some compressed formats where
exact no-decode evidence may exist.

## 17. Root-cause classification

Primary classification:

```text
exact_extent_available_but_ignored
```

More precise boundary statement:

```text
AveMediaBridge publishes 105840000 as unsafe_estimated even though bounded
FFmpeg stream duration and Ogg EOS granule already prove 105840000. AveVoice
therefore treats the count as provisional and aligns it upward to 105840640.
```

Rejected causes:

- `duration_to_frames_rounding_error`: all rational candidates equal `105840000`;
- `decoder_skip_discard_not_reflected_in_probe`: skip/discard is zero;
- `container_duration_includes_padding`: format/stream duration equals final;
- `vorbis_granule_semantics`: final granule equals decoded PCM output;
- `resampler_frame_count_difference`: resampler input/output are equal;
- `writer_accounting_error`: writer bytes divide exactly into `105840000` frames;
- `ffmpeg_version_specific_behavior`: linked and CLI agree on the fixture extent.

## 18. Recommended owner and future contract

Recommended owner repo:

```text
AveMediaBridge
```

Recommended future seam:

```text
src/Probe/FrameCountPolicy.cpp
src/Probe/MediaProbeService.cpp
```

Recommended invariant:

```text
For Ogg/Vorbis with one serial stream, valid EOS, no truncation/chaining, and
stream duration or EOS granule in sample-frame time base matching decode output,
publish decodedSampleFrames as authoritative/exact or a typed Ogg-granule exact
authority.
```

The existing public API already has exactness fields. A future API can make the
contract clearer with an explicit value type:

```cpp
struct AudioTimelineExtent {
    std::uint64_t frames;
    AudioTimelineExtentKind kind;
    bool exact;
    bool mayShrink;
    bool mayGrow;
};
```

For this fixture, the preferred fix is not in decoder, resampler, writer, or
AveVoice camera code. AveVoice behaved consistently with the provisional flag it
received.

## 19. AveVoice downstream consequence

AveVoice's provisional Loading path:

```text
ProbeToJson decodedSampleFrames=105840000, trust=unsafe_estimated
-> WaveformPeakSourceHelpers::readProgressiveViewportProbeEstimate
-> alignFramesToPyramidGrid(105840000)
-> provisional sourceFrames=105840640
-> final import frames=105840000
-> same-media reconciliation by -640
```

No AveVoice files were changed during this audit.

## 20. What was proved

- Current AveMediaBridge does not compute or publish `105840640` for the exact
  Ogg/Vorbis fixture.
- The current public probe/preflight value is `105840000` with
  `unsafe_estimated` trust.
- The downstream `105840640` value is exactly AveVoice's 1024-frame provisional
  grid alignment of `105840000`.
- Ogg EOS granule, stream duration, decoder output, resampler output, and writer
  frames all agree on `105840000`.
- The `640` frames are not codec padding, skip/discard, granule trim, rounding,
  resampler behavior, or writer error.

## 21. What remains unproved

- A product fix was intentionally not implemented.
- The audit does not yet prove a safe authoritative policy for every possible
  Ogg/Vorbis edge case such as chained streams, missing EOS, truncated files, or
  non-zero initial granules.
- The cross-format controls are representative local fixtures, not an exhaustive
  corpus certification.
- A future policy change must add regression coverage before promoting
  Ogg/Vorbis probe evidence to authoritative.

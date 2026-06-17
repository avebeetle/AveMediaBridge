# AveMediaBridge Frame Count Policy

Status: production behavior to preserve during refactor.

## Why This Exists

AveVoice progressive waveform loading needs a stable full-file frame space while import chunks arrive. A fast probe frame estimate that later differs from final decoded PCM frames can cause visible Loading/Ready shrink, growth, or rebuild.

AveMediaBridge therefore classifies `decodedSampleFrames` by trust:

- `authoritative`: accepted as final decoded frame count;
- `aligned_authoritative`: accepted as frame-count evidence that aligns with final decoded geometry;
- `unsafe_estimated`: useful as a provisional Loading/layout estimate, but not final truth;
- `unknown`: no usable count.

The probe JSON keeps both the frame count and its policy evidence so AveVoice can distinguish final authority from provisional extent.

## JSON Fields

Probe JSON frame-count fields include:

- `decodedSampleFrames`
- `decodedSampleFramesKind`
- `decodedSampleFramesTrust`
- `decodedSampleFramesSource`
- `decodedSampleFramesBeforeCorrection`
- `decodedSampleFramesBeforeGaplessCorrection`
- `packetPtsSpanFrames`
- `packetDurationSumFrames`
- `packetFrameCountCandidateUsed`
- `frameCountPolicyReason`
- `skipSamplesStart`
- `skipSamplesEnd`
- `skipSamplesTotal`
- `gaplessCorrectionApplied`
- `gaplessCorrectionSource`
- `gaplessCorrectedDecodedSampleFrames`

These fields are additive compatibility data. Do not remove or rename them without coordinating AveVoice.

## Authoritative Versus Provisional

Authoritative decoded frame counts are exact enough to define final source length. AveVoice should not align them upward or replace them with duration-derived estimates.

Unsafe estimates may still be useful for progress UI or provisional Loading layout. They must not be presented as final-compatible frame counts.

Coverage is always driven by real decoded chunks. The policy never creates fake silence, fake samples, or artificial right-tail audio.

## Current Codec And Container Rules

### MP3

Two-sided MP3 gapless side data:

- source: FFmpeg `AV_PKT_DATA_SKIP_SAMPLES`;
- policy: subtract start skip and end discard when the correction is safe;
- trust: `authoritative`;
- source string: `mp3_gapless_skip_samples`.

Bare MP3 one-sided start skip:

- applies only when the selected codec is MP3 and the container/demuxer format is bare `mp3`;
- subtracts `skipSamplesStart` when `skipSamplesEnd == 0`;
- trust: `authoritative`;
- reason: `bare_mp3_one_sided_start_skip_applied`.

MP3 inside MP4/MOV/M4A/3GP/3G2:

- does not apply duration-estimate minus one-sided skip as authoritative;
- may use full packet scan candidate `mp3_packet_count_minus_skip_start` when packet count and skip evidence produce the exact decoded frame count;
- trust: `authoritative`;
- reason: `mp4_mp3_packet_count_minus_skip_start_used`.

MP3 frame size is sample-rate aware. Normal MPEG audio packet counts use 1152 samples per packet, while low-rate MP3 can use 576.

### ADTS AAC

Raw ADTS AAC duration can be bitrate-estimated and badly wrong. For format `aac` with AAC codec:

- prefer packet-derived frame counts;
- use packet duration sum or packet count evidence when sane;
- source string: `aac_adts_packet_duration_sum` when duration sum is selected;
- trust: `authoritative`;
- reason: `adts_aac_packet_count_or_duration_used`.

AAC packet frame-size fallback uses 1024 samples because that is the codec frame size, not a sample-rate assumption.

### Opus

For Opus in Ogg/OGA/WebM/Matroska/MKA/WEBA:

- use gapless skip/discard or packet-timeline evidence that matches final decoded frames;
- source string: `opus_gapless_skip_discard`;
- trust: `authoritative`;
- reason: `opus_gapless_or_packet_timeline_used`.

### MPEG-TS, M2TS, And MTS AAC

For MPEG-TS-family containers with AAC:

- use `packetPtsSpanFrames` when a full packet scan has valid, monotonic PTS evidence;
- do not reject a good PTS span only because packet duration sum is poor in the 90 kHz transport time base;
- source string: `packet_pts_span`;
- reason: `mpegts_packet_pts_span_used`.

PTS span conversion uses the selected stream time base and actual stream sample rate.

### MPEGPS MP2

For MPEGPS with selected MP2 audio:

- use packet count times MP2 frame size when sane;
- source string: `mpegps_mp2_packet_count`;
- trust: `authoritative`;
- reason: `mpegps_mp2_packet_count_used`.

MP2 1152 is a codec frame size, not a sample-rate assumption.

Generated MPEGPS validation media may still contain probe-noisy bogus streams. Keep stream-selection changes narrow; stable selected MP2 frame count is the production requirement.

### WMAV2 In ASF/WMA/WMV

For WMAV2 in ASF-family containers:

- compute packet-count candidate minus one decoder delay frame;
- source string: `wmav2_packet_count_minus_decoder_delay`;
- trust: `authoritative`;
- reason: `wmav2_packet_count_minus_decoder_delay_used`.

WMAV2 frame size is sample-rate aware:

```text
sampleRate <= 16000 -> 512
sampleRate <= 22050 -> 1024
sampleRate >  22050 -> 2048
```

### PCM

PCM stream duration in sample-frame time base can be exact:

- source string: `exact_pcm_stream_duration`;
- trust: `authoritative`.

### CAF/ALAC

CAF/ALAC remains non-authoritative when no exact no-decode candidate is available.

Do not fake an exact frame count from a near duration estimate. Keep it `unsafe_estimated` until a proven cheap candidate or decode-derived count is available.

## Sample-Rate Rules

No policy should hardcode sample rate as a proxy for frame count. Frame count conversion must use:

- actual selected stream sample rate;
- selected stream time base for timestamp conversion;
- codec frame sizes only where the codec defines them.

Constants such as 1024, 1152, and 2048 are codec frame sizes. They are not assumptions that media is 44.1 kHz or 48 kHz.

## Refactor Safety

When extracting this policy into `src/Probe/FrameCountPolicy.hpp/.cpp`, keep the input/output value model explicit:

- probe metadata;
- selected codec/container identity;
- packet scan data;
- gapless scan data;
- final `decodedSampleFrames*` fields.

Regression comparison should include every JSON field listed above for representative short and long files. A refactor is not complete if the values change without an intentional behavior-change stage.


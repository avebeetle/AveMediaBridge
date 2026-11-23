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

The optional top-level Boolean `hasVideoTrack` reports whether the discovered
streams include a video track without the attached-picture disposition. A cover
image may appear as `video` in the `streams` list while this flag is false. Older
probes and results without a stream inventory omit the field.
The result describes the bundled FFmpeg stream inventory. For example, this
audio-oriented build reports audio streams only in the MPEG-PS and VOB samples
50 and 51, where a full FFmpeg build also identifies MPEG-2 video.

## Authoritative Versus Provisional

Authoritative decoded frame counts are exact enough to define final source length. AveVoice should not align them upward or replace them with duration-derived estimates.

Unsafe estimates may still be useful for progress UI or provisional Loading layout. They must not be presented as final-compatible frame counts.

Coverage is always driven by real decoded chunks. The policy never creates fake silence, fake samples, or artificial right-tail audio.

### CAF with Apple Lossless packets

The optional pinned-source CAF inspector can establish the initial scale during the
ordinary SHA-256 pass. For supported mono and stereo ALAC layouts it checks the
CAF audio description, magic cookie, packet table and data boundaries. It sums
the decoded frame counts from **every** packet header. An explicit short packet
can occur anywhere in the stream. A nominal count multiplied by packet capacity
can overstate the decoded length even in a 40-minute recording.

The bounded metadata lookup accepts unedited data with zero priming and remainder.
If the layout is unsupported, the caller uses its existing estimate and the importer
still decodes through EOF. A late rejection after the source hash has finished
can return that hash alone. See the C ABI in
[`AveMediaBridgeSourceInspectApi.h`](../include/AveMediaBridge/AveMediaBridgeSourceInspectApi.h).

## Exact Packet Presentation Evidence

A complete shared demux traversal may establish a stronger initial authority than rounded container duration metadata when all of the following are known in one sample domain:

- every selected packet contributes a known codec-frame sample count;
- packet and gapless accumulators both reach EOF without warnings;
- initial skip and terminal discard come from FFmpeg/container gapless evidence;
- checked subtraction leaves a positive presentation frame count.

The resulting `exact_packet_presentation` count is the physical codec-frame sum minus authoritative leading skip and terminal discard. Raw duration-derived frames remain in `decodedSampleFramesBeforeCorrection` for provenance. Incomplete scans, unknown zero padding, conflicting evidence, and arithmetic boundary failures remain provisional.

Packet timing and gapless metadata continue to use independent accumulators fed by one physical reader loop. The exact count controls the initial Loading presentation extent; codec skip is still applied only by FFmpeg during decode.

Exact resolution is evaluated after that shared traversal whenever both evidence
components were already collected. Preliminary codec-parameter padding may
request the traversal, but it is not an eligibility gate after packet side data
and sample-exact terminal duration evidence are available. Accepted exact
authority takes precedence over legacy packet-count estimates; rejected or
incomplete evidence retains the existing provisional and typed-reconcile paths.

## Current Codec And Container Rules

### Matroska video with AAC audio

When only the movie duration is known, the native importer can use it for
progress. It checks actual PCM writes as usual. A longer picture track cannot
cause a disk-space refusal for the selected audio based on that duration.
An independently established audio estimate retains its usual preflight checks.

### Matroska/WebM video with Vorbis audio

For a seekable file up to 1 MiB with a finite, complete Matroska segment, one video
track and one mono/stereo Vorbis track, the probe may count the selected packets
using the native Vorbis overlap sizes. The first packet primes the decoder.
The pass accepts millisecond timestamps starting at zero, continuous to within
one tick, without packet side data, padding, discarded packets or codec changes.
It stops after 16,384 demuxed packets or 120 seconds of selected audio. On a
clean EOF, `matroska_vorbis_packet_presentation` supplies the Loading extent.

The probe owns this demuxer. Import opens a fresh reader, decodes through EOF
and publishes actual PCM, including its final frame. The packet count cannot
be used as a decoder trimming or padding budget, even if the probed packets
and the output of a damaged stream differ.

For inputs outside the scan bounds, a selected stream duration takes priority.
If only the movie duration is available, a canonical zero-origin
`DURATION` tag on the selected audio track supplies a provisional estimate
(`selected_track_duration_tag_estimate`). Neither this tag nor the whole
movie duration can gate free disk space in this two-track profile. A valid tag
can supply progress even if its declared size exceeds available disk space.
Import preflight reads the metadata without consuming packets. Actual PCM writes
still report disk errors.

### Audio-only Ogg Vorbis

For an Ogg input with one selected Vorbis stream, the initial probe checks the
constituent logical streams against the open media handle. The final granule
of one logical stream cannot establish the length of a concatenation. When
sample rates differ, the result is the accumulated duration rounded at link
boundaries in the document's output sample rate. Its JSON authority source is
`ogg_vorbis_link_presentation` and its domain is `document_samples`.

A single link with a verified ordinary start, matching native shape and exact
frame count keeps the FFmpeg decoder. A single multichannel link whose first
audible page also ends the stream uses the verified link length with that same
decoder. A separate page containing only the priming packet contributes zero
frames. Mixed mono/stereo links use the measured
link boundaries for PCM and the waveform. A change to an unsupported channel
layout gets a distinct refusal. Reaching the bounded probe's read, seek or link
limit reports `quick_scan_limit` and leaves the length unknown.

Ordinary and guarded path imports use the same open handle for measurement and
decoding. After `quick_scan_limit`, callers may pass an immutable, pinned Ogg
input to `AveMediaBridge_ReaderPrepareVorbisV1`. This optional call reads the
logical streams sequentially, counts Vorbis overlap and granule trimming, and
checks page integrity. Cancellation interrupts the scan at read boundaries.
Comment text is discarded as it is validated. The plan supports up to 65,536
logical streams and compressed audio or setup packets up to 4 MiB each. A
successful prepared reader supplies both the authoritative probe and the import
from the same source. The original prepare entry point retains its MOV/AAC
scope. Ogg containers with a separate video stream retain their FFmpeg route.

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

### FLAC STREAMINFO

For FLAC in any container where FFmpeg exposes the canonical 34-byte
STREAMINFO payload through `AVCodecParameters::extradata`, a validated positive
`total_samples` value is sample-exact codec-level presentation evidence.

The parser validates STREAMINFO sample rate and channel count against known
codec parameters. Zero, malformed, truncated, or conflicting metadata remains
unknown. Rounded container duration is retained as diagnostic provenance and
does not override an independently exact STREAMINFO total. This metadata path
does not require a packet scan or decoder open.

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

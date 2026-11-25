"""Check the ALAC sample index against an independently generated lossless waveform."""
from array import array
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave

from Mp4AacIndexImport import audio_sample_ranges, child


SOURCE = "mp4_alac_sample_table_presentation"
MUXERS = (("mp4", ".mp4"), ("ipod", ".m4a"), ("mov", ".mov"))


def signal(frames, channels, depth):
    # Include both signed extrema and a non-periodic distinction between channels.
    maximum = 1 << (depth - 1)
    values = [((i * 9973 + (i // 89) * 41 + 1377) % (2 * maximum)) - maximum
              for i in range(frames * channels)]
    values[0] = -maximum
    if len(values) > 1:
        values[-1] = maximum - 1
    pcm = array("h", values).tobytes() if depth == 16 else b"".join(
        value.to_bytes(3, "little", signed=True) for value in values)
    f32 = array("f", (value / maximum for value in values)).tobytes()
    return pcm, f32


def replace_box_payload(data, path, payload):
    # All size-changing edits use moov after mdat, so they cannot shift PCM packets.
    boxes_to_resize = []
    begin, end = 0, len(data)
    for kind in path:
        start, end = child(data, begin, end, kind)
        boxes_to_resize.append((start, end))
        begin = start + 8
    assert boxes_to_resize[0][0] >= child(data, 0, len(data), b"mdat")[1]
    output = bytearray(data)
    output[start + 8:end] = payload
    delta = len(output) - len(data)
    for start, end in boxes_to_resize:
        struct.pack_into(">I", output, start, end - start + delta)
    return output


def replace_audio_timing(data, rows):
    payload = struct.pack(">II", 0, len(rows)) + b"".join(struct.pack(">II", n, d) for n, d in rows)
    return replace_box_payload(data, (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stts"), payload)


def set_edit(data, duration, media_start=0, leading=0, track_delta=0, movie_delta=0):
    # Change the movie/track headers as well as elst; the media samples and mdhd stay intact.
    entries = ([(leading, -1)] if leading else []) + [(duration, media_start)]
    payload = struct.pack(">II", 0, len(entries)) + b"".join(
        struct.pack(">IiI", ticks, start, 0x10000) for ticks, start in entries)
    output = replace_box_payload(data, (b"moov", b"trak", b"edts", b"elst"), payload)
    moov, end = child(output, 0, len(output), b"moov")
    mvhd, _ = child(output, moov + 8, end, b"mvhd")
    trak, end = child(output, moov + 8, end, b"trak")
    tkhd, _ = child(output, trak + 8, end, b"tkhd")
    assert output[mvhd + 8] == output[tkhd + 8] == 0
    struct.pack_into(">I", output, mvhd + 24, duration + leading + movie_delta)
    struct.pack_into(">I", output, tkhd + 28, duration + leading + track_delta)
    return output


def shorten_declared_duration(data, frames):
    # The exact-timescale FFmpeg file uses v0 movie, track, media and edit headers.
    # Make their declarations agree, while its last lossless packet is untouched.
    output = replace_audio_timing(data, [(3, 4096), (1, 2)])
    for path, duration_offset in (((b"moov", b"mvhd"), 24),
                                  ((b"moov", b"trak", b"tkhd"), 28),
                                  ((b"moov", b"trak", b"mdia", b"mdhd"), 24),
                                  ((b"moov", b"trak", b"edts", b"elst"), 16)):
        begin, end = 0, len(output)
        for tag in path:
            start, end = child(output, begin, end, tag)
            begin = start + 8
        offset = start + duration_offset
        assert output[start + 8] == 0 and struct.unpack_from(">I", output, offset)[0] == frames
        struct.pack_into(">I", output, offset, frames - 1)
    return output


def fixtures(root, ffmpeg):
    cases = []

    def ff(*arguments):
        subprocess.run([str(ffmpeg), "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                        *map(str, arguments)], check=True)

    def add(path, frames, channels, rate, depth, behavior):
        cases.append(dict(name=path.name, frames=frames, channels=channels, rate=rate,
                          depth=depth, behavior=behavior))

    def encode(name, mux, frames, rate, channels, front, depth=16, scale=None, extra=(), behavior="index"):
        wav = root / "input.wav"
        with wave.open(str(wav), "wb") as w:
            w.setparams((channels, depth // 8, rate, frames, "NONE", "not compressed"))
            w.writeframes(signal(frames, channels, depth)[0])
        path = root / name
        flags = ["-movflags", "+faststart"] if front else []
        ff("-i", wav, "-c:a", "alac", "-movie_timescale", scale or rate, *flags, *extra, "-f", mux, path)
        add(path, frames, channels, rate, depth, behavior)
        return path

    lengths = (1, 2, 4095, 4096, 4097, 8191, 8192, 8193, 65539)
    for m, (mux, suffix) in enumerate(MUXERS):
        for i, frames in enumerate(lengths):
            rate = (44100, 48000, 32000)[(i + m) % 3]
            channels = 1 + (i + m) % 2
            encode(f"{mux}-{frames}{suffix}", mux, frames, rate, channels, (i + m) % 2 == 0)
    encode("stereo-24bit.mov", "mov", 8201, 48000, 2, False, depth=24)

    exact = encode("front-65539.mp4", "mp4", 65539, 48000, 2, True)
    encode("rounded-8193.m4a", "ipod", 8193, 48000, 2, True, scale=1000)
    for i, n in enumerate((1, 2, 47, 48, 49, 4103, 4104, 4105, 8207, 8208, 8209)):
        mux, ext = MUXERS[i % len(MUXERS)]
        encode(f"ms-{n}{ext}", mux, n, 48000, 1 + i % 2, i % 2 == 0, scale=1000)
    encode("ms-fractional-rate.mov", "mov", 8193, 44100, 2, True, scale=1000)
    encode("fine-movie-clock.mp4", "mp4", 8193, 48000, 2, True, scale=90000, behavior="fallback")
    encode("coarse-edit.m4a", "ipod", 8193, 48000, 2, True, scale=600, behavior="fallback")
    edits = encode("ms-control.mp4", "mp4", 8207, 48000, 1, False, scale=1000)
    for name, params in (("excess-millisecond", {"duration":172}),
                         ("shorter-millisecond", {"duration":170}),
                         ("nonzero-start", {"duration":171, "media_start":1}),
                         ("leading-empty-edit", {"duration":171, "leading":2}),
                         ("wrong-track-end", {"duration":171, "track_delta":1}),
                         ("movie-ends-early", {"duration":171, "movie_delta":-1})):
        changed = root / (name + ".mp4")
        changed.write_bytes(set_edit(edits.read_bytes(), **params))
        add(changed, 8207, 1, 48000, 16, "fallback")
        if name == "shorter-millisecond":
            cases[-1]["presentation"] = (0, 170 * 48)
        if name == "nonzero-start":
            cases[-1]["presentation"] = (1, 8207)
    encode("fragmented-8193.mp4", "mp4", 8193, 48000, 1, False,
           extra=("-movflags", "+frag_keyframe+empty_moov"), behavior="fallback")

    data = exact.read_bytes()
    packets = audio_sample_ranges(data)
    mdat, mdat_end = child(data, 0, len(data), b"mdat")
    assert packets[-1][1] == mdat_end == len(data)
    for name, cut in (("whole-last", packets[-1][0]), ("partial-last", packets[-1][1] - 1),
                      ("half-of-audio", packets[len(packets) // 2][0])):
        for declared, length in (("original", mdat_end - mdat), ("to-eof", 0), ("actual", cut - mdat)):
            damaged = root / f"{name}-{declared}.mp4"
            contents = bytearray(data[:cut])
            struct.pack_into(">I", contents, mdat, length)
            damaged.write_bytes(contents)
            add(damaged, 65539, 2, 48000, 16, "reject")

    video = root / "longer-picture.mp4"
    ff("-f", "lavfi", "-i", "color=s=32x32:r=10:d=3", "-i", exact,
       "-map", "0:v", "-map", "1:a", "-c:v", "mpeg4", "-c:a", "copy",
       "-movie_timescale", 48000, "-movflags", "+faststart", "-f", "mp4", video)
    add(video, 65539, 2, 48000, 16, "index")
    data = video.read_bytes()
    audio_end = max(end for _, end in audio_sample_ranges(data))
    assert audio_end < len(data) - 16
    cut_video = root / "complete-audio-cut-video.mp4"
    cut_video.write_bytes(data[:audio_end])
    add(cut_video, 65539, 2, 48000, 16, "complete")

    normal = encode("physical.m4a", "ipod", 12291, 48000, 1, False)
    trimmed = root / "one-sample-edit.m4a"
    trimmed.write_bytes(set_edit(normal.read_bytes(), 12290))
    add(trimmed, 12291, 1, 48000, 16, "fallback")
    cases[-1]["presentation"] = (0, 12290)
    shifted = root / "shifted-interior.m4a"
    shifted.write_bytes(replace_audio_timing(normal.read_bytes(), [(1, 4095), (1, 4097), (1, 4096), (1, 3)]))
    add(shifted, 12291, 1, 48000, 16, "reject_or_fallback")
    shorter = root / "one-more-physical-frame.m4a"
    shorter.write_bytes(shorten_declared_duration(normal.read_bytes(), 12291))
    add(shorter, 12291, 1, 48000, 16, "reject_or_fallback")

    parts = [1025, 4097, 8193]
    segments = [encode(f"part-{n}.m4a", "ipod", n, 48000, 1, False) for n in parts]
    playlist = root / "inputs.ffconcat"
    playlist.write_text("ffconcat version 1.0\n" + "".join(f"file '{p.name}'\n" for p in segments))
    joined = root / "short-interior.m4a"
    ff("-f", "concat", "-safe", 0, "-i", playlist, "-c:a", "copy", "-movie_timescale", 48000,
       "-f", "ipod", joined)
    add(joined, sum(parts), 1, 48000, 16, "complete")
    cases[-1]["parts"] = parts
    swapped = root / "short-packet-boundary-shift.m4a"
    swapped.write_bytes(replace_audio_timing(joined.read_bytes(), [(1, 1024), (1, 4096), (1, 2),
                                                                    (2, 4096), (1, 1)]))
    add(swapped, sum(parts), 1, 48000, 16, "reject")
    cases[-1]["parts"] = parts

    for selected_shorter in (False, True):
        first, second = (segments[0], exact) if selected_shorter else (exact, segments[0])
        tracks = root / ("selected-short.mp4" if selected_shorter else "selected-long.mp4")
        ff("-i", first, "-i", second, "-map", "0:a", "-map", "1:a", "-c:a", "copy", "-disposition:a:0",
           "default", "-disposition:a:1", "0", "-movie_timescale", 48000, "-movflags", "+faststart", tracks)
        n, channels = (1025, 1) if selected_shorter else (65539, 2)
        add(tracks, n, channels, 48000, 16, "complete")
        data = tracks.read_bytes()
        if selected_shorter:
            # The simple sample-range helper expects a variable-size stsz, so take
            # the long second track's final bytes off the physical end instead.
            cut = len(data) - 5
        else:
            cut = audio_sample_ranges(data)[-1][0]
        missing = tracks.with_suffix(".cut.mp4")
        missing.write_bytes(data[:cut])
        add(missing, n, channels, 48000, 16, "complete" if selected_shorter else "reject")

    tag = root / "large-comment.txt"
    tag.write_bytes(b";FFMETADATA1\ncomment=" + b"." * (4 * 1024 * 1024 + 128) + b"\n")
    tagged = root / "large-moov.mp4"
    ff("-i", exact, "-f", "ffmetadata", "-i", tag, "-map_metadata", 1, "-c", "copy",
       "-movflags", "+faststart", "-movie_timescale", 48000, tagged)
    data = tagged.read_bytes()
    a, b = child(data, 0, len(data), b"moov")
    assert b - a > 4 * 1024 * 1024
    add(tagged, 65539, 2, 48000, 16, "fallback")
    missing = root / "large-moov-cut.mp4"
    missing.write_bytes(data[:audio_sample_ranges(data)[-1][0]])
    add(missing, 65539, 2, 48000, 16, "reject")
    return cases


def run_imports(root, cases, library, runtime, sessions):
    results = []
    with os.add_dll_directory(str(Path(runtime).resolve())):
        dll = ctypes.CDLL(str(Path(library).resolve()))
        load = dll.AveMediaBridge_ImportAudioToSession
        load.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
        error = dll.AveMediaBridge_GetLastErrorText
        error.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
        for case in cases:
            media = root / case["name"]
            out = sessions / media.name
            rc = load(str(media), str(out))
            row = dict(case, rc=rc)
            if rc:
                message = ctypes.create_unicode_buffer(2048)
                error(message, len(message))
                row.update(error=message.value, clean=not list(out.glob("*")))
            else:
                budget = json.loads((out / "metadata.json").read_bytes())["presentationBudget"]
                info = json.loads((out / "audio_info.json").read_bytes())
                pcm = (out / "original_f32.bin").read_bytes()
                expected = expected_pcm(case)
                row.update(actualFrames=info["frames"], channelsMatch=info["channels"] == case["channels"],
                           rateMatch=info["sampleRate"] == case["rate"], bytes=len(pcm),
                           digest=hashlib.sha256(pcm).hexdigest(), source=budget["source"],
                           packetScan=budget["packetScanExecuted"], reason=budget["reason"],
                           fullMatch=pcm == expected)
            results.append(row)
    return results


def expected_pcm(case):
    pcm = b"".join(signal(n, case["channels"], case["depth"])[1] for n in case.get("parts", [case["frames"]]))
    begin, end = case.get("presentation", (0, case["frames"]))
    stride = case["channels"] * 4
    return pcm[begin * stride:end * stride]


def verify(rows):
    for row in rows:
        kind = row["behavior"]
        if row["rc"]:
            assert kind in ("reject", "reject_or_fallback") and row["clean"], row
            continue
        assert kind != "reject" and row["channelsMatch"] and row["rateMatch"], row
        assert row["actualFrames"] * row["channels"] * 4 == row["bytes"], row
        begin, end = row.get("presentation", (0, row["frames"]))
        assert row["fullMatch"] and row["actualFrames"] == end - begin, row
        if kind == "index":
            assert row["source"] == SOURCE and not row["packetScan"], row
        if kind in ("fallback", "reject_or_fallback"):
            assert row["source"] != SOURCE, row


def main(library, runtime, ffmpeg):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        cases = fixtures(root, ffmpeg)
        rows = run_imports(root, cases, library, runtime, root / "sessions")
        verify(rows)
        print(f"ALAC index: {len(rows)} complete, fallback or correctly rejected cases")


if __name__ == "__main__":
    main(*sys.argv[1:])

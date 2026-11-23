"""Check the selected audio extent independently of the video duration."""
import ctypes
from array import array
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile


class Progress(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("written_frames", ctypes.c_uint64),
                ("written_bytes", ctypes.c_uint64), ("expected_frames", ctypes.c_uint64),
                ("expected_bytes", ctypes.c_uint64), ("end_sec", ctypes.c_double),
                ("fraction", ctypes.c_double), ("rate", ctypes.c_int),
                ("channels", ctypes.c_int), ("flags", ctypes.c_uint32)]


ProgressCallback = ctypes.WINFUNCTYPE(None, ctypes.POINTER(Progress), ctypes.c_void_p)


class Options(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("path", ctypes.c_wchar_p),
                ("session", ctypes.c_wchar_p), ("progress", ProgressCallback),
                ("cancel", ctypes.c_void_p), ("user", ctypes.c_void_p), ("wave", ctypes.c_void_p)]


def padded_segment(source, target):
    data = bytearray(source.read_bytes())
    pos = data.index(bytes.fromhex("18538067")) + 4
    width = 9 - data[pos].bit_length()
    original = int.from_bytes(data[pos:pos + width], "big") ^ (1 << (7 * width))
    assert pos + width + original == len(data)
    padding = 1024 * 1024
    element = b"\xec" + ((1 << 21) + padding).to_bytes(3, "big") + bytes(padding)
    data[pos:pos + width] = ((1 << (7 * width)) + original + len(element)).to_bytes(width, "big")
    target.write_bytes(data + element)


def main(library, ffmpeg_dir, ffmpeg):
    def ff(*args):
        return subprocess.check_output([ffmpeg, "-nostdin", "-v", "error", *map(str, args)])

    encoders = ff("-encoders").decode(errors="replace")
    if " libvorbis " not in encoders or " libvpx " not in encoders:
        return 77
    with os.add_dll_directory(ffmpeg_dir), tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        dll = ctypes.WinDLL(library)
        probe = dll.AveMediaBridge_ProbeToJson
        probe.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p)
        legacy_probe = dll.AveMediaBridge_ProbeFrameCountCandidatesToJson
        legacy_probe.argtypes = probe.argtypes
        decode = dll.AveMediaBridge_ImportAudioToSession
        decode.argtypes = probe.argtypes
        decode_ex = dll.AveMediaBridge_ImportAudioToSessionEx
        decode_ex.argtypes = [ctypes.POINTER(Options)]

        def info(media):
            evidence = root / "probe.json"
            assert probe(str(media), str(evidence)) == 0, media
            return json.loads(evidence.read_text(encoding="utf-8-sig"))

        def check_reopen(media, channels, expected_progress=None):
            samples = array("f", ff("-i", media, "-map", "0:a:0", "-f", "f32le", "pipe:1"))
            first = b""
            for trial in range(2):
                session = root / f"{media.name}-open-{trial}"
                session.mkdir()
                assert legacy_probe(str(media), str(session / "probe.json")) == 0
                if trial == 0:
                    assert decode(str(media), str(session)) == 0, media
                else:
                    estimates = []
                    def capture(state, _):
                        estimates.append(state.contents.expected_frames)
                    options = Options(ctypes.sizeof(Options), str(media), str(session), ProgressCallback(capture))
                    assert decode_ex(ctypes.byref(options)) == 0, media
                    if expected_progress is not None:
                        assert set(estimates) == {expected_progress}, (media.name, estimates, expected_progress)
                pcm = (session / "original_f32.bin").read_bytes()
                assert len(pcm) == len(samples) * 4 and len(pcm) % (4 * channels) == 0
                if trial == 0:
                    first = pcm
                    assert max(abs(a-b) for a,b in zip(samples, array("f", pcm))) < 1e-4
                else:
                    assert pcm == first
                meta = json.loads((session / "metadata.json").read_text(encoding="utf-8-sig"))
                assert not meta["presentationBudget"]["known"], meta["presentationBudget"]

        for ext, channels, count in [("mkv", 1, 1025), ("webm", 2, 44101)]:
            # Encode separately so changes to the video cannot re-encode the audio.
            track = root / f"{ext}.mka"
            ff("-f", "lavfi", "-i", "sine=frequency=763:sample_rate=44100",
               "-af", f"atrim=end_sample={count}", "-ac", channels, "-c:a", "libvorbis", track)
            media = root / f"ordinary.{ext}"
            ff("-f", "lavfi", "-i", "color=s=32x32:r=10:d=4", "-i", track,
               "-map", "0:v", "-map", "1:a", "-c:v", "libvpx", "-c:a", "copy", "-write_crc32", 0, media)
            decoded = ff("-i", media, "-map", "0:a:0", "-f", "f32le", "pipe:1")
            frames = len(decoded) // (4 * channels)
            doc = info(media)
            assert (doc["decodedSampleFramesSource"], doc["decodedSampleFrames"]) == (
                "matroska_vorbis_packet_presentation", frames), (media.name, frames,
                doc["decodedSampleFramesSource"], doc["decodedSampleFrames"])
            assert doc["decodedSampleFramesKind"] == "exact"
            print(f"{ext}: {frames} native frames", flush=True)
            check_reopen(media, channels)

            padded = root / f"large.{ext}"
            padded_segment(media, padded)
            fallback = info(padded)
            assert fallback["decodedSampleFramesSource"] == "selected_track_duration_tag_estimate", fallback
            assert fallback["decodedSampleFramesTrust"] == "unsafe_estimated"
            assert abs(fallback["decodedSampleFrames"] - frames) < 2048
            check_reopen(padded, channels, fallback["decodedSampleFrames"])

            no_tags = root / f"no-tags.{ext}"
            original = padded.read_bytes()
            assert original.count(b"DURATION") == 2
            no_tags.write_bytes(original.replace(b"DURATION", b"DISABLED"))
            without = info(no_tags)
            assert without["decodedSampleFramesSource"] == "format_duration_estimate", without
            check_reopen(no_tags, channels, 0)

            clipped = root / f"clipped.{ext}"
            clipped.write_bytes(media.read_bytes()[:-12])
            assert info(clipped)["decodedSampleFramesKind"] != "exact"

        gap = root / "gap.webm"
        ff("-i", media, "-map", "0", "-c", "copy", "-bsf:a",
           "setts=pts='PTS+gte(N,5)*100':dts='DTS+gte(N,5)*100'", gap)
        assert info(gap)["decodedSampleFramesKind"] != "exact"
        check_reopen(gap, channels)

        # Over 16,384 demuxed packets, while small enough to enter the scan.
        dense = root / "dense.webm"
        ff("-f", "lavfi", "-i", "color=s=16x16:r=250:d=68", "-i", track,
           "-map", "0:v", "-map", "1:a", "-c:v", "libvpx", "-deadline", "realtime",
           "-cpu-used", 8, "-threads", 1, "-c:a", "copy", dense)
        assert dense.stat().st_size < 1024 * 1024
        assert info(dense)["decodedSampleFramesSource"] == "selected_track_duration_tag_estimate"
        check_reopen(dense, channels)

        multi = root / "multi.mkv"
        ff("-i", media, "-map", "0:v", "-map", "0:a", "-map", "0:a", "-c", "copy", multi)
        assert info(multi)["decodedSampleFramesKind"] != "exact"

        # Both tags fit the declared movie end, but the complete selected audio
        # is only a second long. Its metadata implies roughly 118 GiB of PCM.
        # Check the actual writer rather than allocating that amount of data.
        oversized = root / "oversized-tag.webm"
        source = bytearray(padded.read_bytes())
        duration = source.index(b"\x44\x89\x88") + 3
        struct.pack_into(">d", source, duration, 99 * 3600000.0)
        audio_tag = source.rindex(b"DURATION")
        tag = source.index(b"00:", audio_tag)
        source[tag:tag + 18] = b"98:59:59.000000000"
        oversized.write_bytes(source)
        over = info(oversized)
        assert over["decodedSampleFramesSource"] == "selected_track_duration_tag_estimate", over
        assert over["decodedSampleFrames"] * 8 > 120_000_000_000
        check_reopen(oversized, channels, over["decodedSampleFrames"])
    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))

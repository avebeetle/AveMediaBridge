"""Compare indexed MOV PCM, edited presentations and truncated files with known samples."""
from array import array
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave

from Mp4AacIndexImport import child
from Mp4AlacIndexImport import run_imports, set_edit, signal

SOURCE = "mov_pcm_sample_table_presentation"


def fixtures(root, ffmpeg):
    cases = []

    def encode(name, frames, rate=44100, channels=2, codec="pcm_s16le", front=False,
               movie_scale=1000, video=False):
        with wave.open(str(root / "tone.wav"), "wb") as wav:
            wav.setparams((channels, 2, rate, frames, "NONE", "not compressed"))
            wav.writeframes(signal(frames, channels, 16)[0])
        command = [ffmpeg, "-v", "error", "-nostdin", "-y", "-i", str(root / "tone.wav")]
        if video:
            command += ["-f", "lavfi", "-i", "color=s=32x32:r=10:d=2", "-map", "1:v",
                        "-map", "0:a", "-c:v", "mpeg4"]
        command += ["-c:a", codec, "-movie_timescale", str(movie_scale)]
        if front:
            command += ["-movflags", "+faststart"]
        path = root / name
        subprocess.run([*command, "-f", "mov", str(path)], check=True)
        cases.append(dict(name=name, frames=frames, rate=rate, channels=channels,
                          depth=16, behavior="index"))
        return path

    for i, count in enumerate((1, 2, 43, 44, 45, 1023, 1024, 1025, 4095, 4096, 4097, 44101)):
        encode(f"pcm-{count}.mov", count, channels=1 + i % 2, front=bool(i % 2),
               codec=("pcm_s16le", "pcm_s16be")[i % 2])
    encode("video-longer.mov", 8201, front=True, video=True)
    normal = encode("native.mov", 12291, rate=48000, movie_scale=48000)
    for name, start, end in (("head", 1, 12291), ("tail", 0, 12290), ("both", 33, 11289),
                             ("penultimate-only", 12289, 12290), ("last-only", 12290, 12291)):
        path = root / (name + ".mov")
        path.write_bytes(set_edit(normal.read_bytes(), end - start, media_start=start))
        cases.append(dict(cases[-1], name=path.name, behavior="edited", presentation=(start, end)))
    complex_edit = root / "leading-empty-edit.mov"
    complex_edit.write_bytes(set_edit(normal.read_bytes(), 12291, leading=240))
    cases.append(dict(name=complex_edit.name, frames=12291, rate=48000, channels=2, depth=16,
                      behavior="fallback"))
    for codec, width in (("pcm_u8", 1), ("pcm_s24le", 3), ("pcm_s32le", 4),
                         ("pcm_f32le", 4), ("pcm_f64le", 8)):
        path = encode(codec + ".mov", 8193, codec=codec, front=True)
        cases[-1]["codec"] = codec
        stride = 2 * width
        cut = root / ("missing-last-" + codec + ".mov")
        cut.write_bytes(path.read_bytes()[:-stride])
        cases.append(dict(cases[-1], name=cut.name, behavior="reject"))
    front = encode("front.mov", 8193, rate=48000, front=True)
    data = front.read_bytes()
    begin, end = child(data, 0, len(data), b"mdat")
    assert end == len(data) and end - begin >= 8193 * 4
    for cut in (end - 1, end - 4, end - 4096):
        path = root / f"cut-{end - cut}.mov"
        contents = bytearray(data[:cut])
        struct.pack_into(">I", contents, begin, len(contents) - begin)
        path.write_bytes(contents)
        cases.append(dict(cases[-1], name=path.name, behavior="reject"))
    replaced = bytearray(data)
    struct.pack_into(">I", replaced, begin, end - begin - 16)
    struct.pack_into(">I4s8x", replaced, end - 16, 16, b"free")
    path = root / "last-frames-in-free.mov"
    path.write_bytes(replaced)
    cases.append(dict(cases[-1], name=path.name, behavior="reject"))
    chosen = encode("edit-source.mov", 10025, rate=44100, front=True, movie_scale=44100)
    edited = bytearray(chosen.read_bytes())
    for path, offset in (((b"moov", b"mvhd"), 24), ((b"moov", b"trak", b"tkhd"), 28),
                         ((b"moov", b"trak", b"edts", b"elst"), 16)):
        a, b = 0, len(edited)
        for tag in path:
            start, b = child(edited, a, b, tag)
            a = start + 8
        struct.pack_into(">I", edited, start + offset, 1405)
        if path[-1] == b"elst":
            struct.pack_into(">I", edited, start + offset + 4, 2051)
    mdat, limit = child(edited, 0, len(edited), b"mdat")
    payload = mdat + 8
    for extra in (-1, 0, 1, 4096):
        avail = payload + 3456 * 4 + extra
        for layout in ("physical", "free"):
            clipped = bytearray(edited[:avail])
            if layout == "free":
                struct.pack_into(">I", clipped, mdat, avail - mdat)
                clipped.extend(struct.pack(">I4s", limit - avail, b"free"))
                clipped.extend(bytes(limit - len(clipped)))
            path = root / f"edit-{layout}-extra-{extra}.mov"
            path.write_bytes(clipped)
            cases.append(dict(name=path.name, frames=10025, channels=2, depth=16, rate=44100,
                              presentation=(2051, 3456), behavior="reject" if extra < 0 else "edited"))
    return cases


def verify(rows):
    for row in rows:
        if row["behavior"] == "reject":
            assert row["rc"] and row["clean"], row
            continue
        if row.get("codec") == "pcm_u8":
            values = array("h")
            values.frombytes(signal(row["frames"], row["channels"], 16)[0])
            reference = array("f", ((value >> 8) / 128 for value in values))
            row["fullMatch"] = hashlib.sha256(reference.tobytes()).hexdigest() == row.get("digest")
        assert row["rc"] == 0 and row["channelsMatch"] and row["rateMatch"] and row["fullMatch"], row
        begin, end = row.get("presentation", (0, row["frames"]))
        assert row["actualFrames"] == end - begin, row
        if row["behavior"] == "fallback":
            assert row["source"] != SOURCE and row["packetScan"], row
        else:
            assert row["source"] == SOURCE and not row["packetScan"], row


def main(library, runtime, ffmpeg):
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        cases = fixtures(root, ffmpeg)
        verify(run_imports(root, cases, library, runtime, root / "sessions"))
        print(f"MOV PCM: {len(cases)} indexed, edited and truncated inputs checked")


if __name__ == "__main__":
    main(*sys.argv[1:])

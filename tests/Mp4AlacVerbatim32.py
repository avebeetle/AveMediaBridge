"""Exercise real 32-bit ALAC verbatim elements through the ordinary import ABI."""
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


BLOCK = 4096
RATE = 48000


def variable_length(n):
    groups = [n & 0x7f]
    while n >= 128:
        n >>= 7
        groups.append(0x80 | (n & 0x7f))
    return bytes(reversed(groups))


def element(samples, channels, compressed=False):
    count = len(samples) // channels
    short = count != BLOCK
    # Element ID (SCE or CPE), instance, twelve reserved bits, explicit-size
    # flag, zero shifted bytes, then the verbatim flag. All fields are MSB-first.
    width = 23
    bits = ((channels - 1) << 20) | (int(short) << 3) | int(not compressed)
    if short:
        bits = (bits << 32) | count
        width += 32
    payload = struct.pack(">" + "i" * len(samples), *samples)
    bits = (bits << (len(payload) * 8)) | int.from_bytes(payload, "big")
    width += len(payload) * 8
    bits = (bits << 3) | 7  # ID_END
    width += 3
    return (bits << (-width % 8)).to_bytes((width + 7) // 8, "big")


def chunk(tag, payload):
    return tag + struct.pack(">Q", len(payload)) + payload


def fixture(root, name, mux, channels, lengths, defect, ffmpeg):
    rows, packets = [], []
    for index, count in enumerate(lengths):
        samples = [((i * 2449879 + 37221 + index * 77581) % 2**32) - 2**31 for i in range(count * channels)]
        samples[0], samples[-1] = -(2**31), 2**31 - 1
        packet = element(samples, channels, compressed=defect == "prediction" and index == 0)
        if defect == "truncated" and index == len(lengths) - 1:
            packet = packet[:-3]
        packets.append(packet)
        rows.extend(samples)
    desc = struct.pack(">d4s5I", RATE, b"alac", 4, 0, BLOCK, channels, 0)
    cookie = struct.pack(">I6BHIII", BLOCK, 0, 32, 40, 10, 14, channels, 255, 0, 0, RATE)
    pakt = struct.pack(">QQII", len(packets), sum(lengths), 0, len(packets) * BLOCK - sum(lengths))
    caf = root / (name + ".caf")
    caf.write_bytes(b"caff\x00\x01\x00\x00" + chunk(b"desc", desc) + chunk(b"kuki", cookie) +
                    chunk(b"pakt", pakt + b"".join(variable_length(len(p)) for p in packets)) +
                    chunk(b"data", bytes(4) + b"".join(packets)))
    dst = root / name
    # A CAF over fixed packet time exposes its final packet duration differently
    # across CLI versions. Use the known, native per-packet presentation in MP4.
    duration = f"if(eq(N\\,{len(lengths) - 1})\\,{lengths[-1]}\\,{BLOCK})"
    subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-y", "-i", str(caf), "-map", "0:a",
                    "-c", "copy", "-bsf:a", "setts=duration=" + duration, "-movie_timescale",
                    str(RATE), "-f", mux, str(dst)], check=True)
    return dst, struct.pack("<" + "f" * len(rows), *(s / 2**31 for s in rows))


def main(library, runtime, ffmpeg):
    with os.add_dll_directory(str(Path(runtime).resolve())), tempfile.TemporaryDirectory() as folder:
        dll = ctypes.CDLL(str(Path(library).resolve()))
        run = dll.AveMediaBridge_ImportAudioToSession
        run.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
        err = dll.AveMediaBridge_GetLastErrorText
        err.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
        root = Path(folder)
        cases = [("whole.mp4", "mp4", 2, [BLOCK, BLOCK], None),
                 ("tail-one.m4a", "ipod", 2, [BLOCK, 1], None),
                 ("tail-two.mov", "mov", 2, [BLOCK, 2], None),
                 ("tail-three.mp4", "mp4", 2, [BLOCK, 3], None),
                 ("single.m4a", "ipod", 2, [1], None),
                 ("mono.mov", "mov", 1, [BLOCK, 1], None),
                 ("prediction.mp4", "mp4", 2, [BLOCK, 1], "prediction"),
                 ("truncated.m4a", "ipod", 2, [BLOCK, 3], "truncated")]
        for name, mux, channels, lengths, defect in cases:
            media, pcm = fixture(root, name, mux, channels, lengths, defect, ffmpeg)
            session = root / ("session-" + name)
            status = run(str(media), str(session))
            error = ctypes.create_unicode_buffer(1024)
            err(error, len(error))
            if defect:
                assert status and not list(session.glob("*")), (name, status, error.value)
                continue
            assert not status, (name, error.value)
            info = json.loads((session / "audio_info.json").read_bytes())
            assert (info["sampleRate"], info["channels"], info["frames"]) == (RATE, channels, sum(lengths)), (name, info)
            assert (session / "original_f32.bin").read_bytes() == pcm, name
        print("ALAC 32-bit verbatim stereo: all 8 cases passed")


if __name__ == "__main__":
    main(*sys.argv[1:])

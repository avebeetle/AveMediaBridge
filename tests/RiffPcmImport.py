"""Verify native WAV budgets and every decoded sample through the public import DLL."""
import ctypes
import json
import os
from pathlib import Path
import struct
import sys
import tempfile


def chunk(tag, data):
    return tag + struct.pack("<I", len(data)) + data + bytes(len(data) % 2)


def fixture(tag, bits, channels, frames, layout):
    width = bits // 8
    align = width * channels
    rate = 44100
    extensible = layout == "extensible"
    fmt = struct.pack("<HHIIHH", 65534 if extensible else tag, channels,
                      rate, rate * align, align, bits)
    if extensible:
        fmt += struct.pack("<HHII", 22, bits, 4 if channels == 1 else 3, tag)
        fmt += bytes.fromhex("00001000800000aa00389b71")
    payload = bytearray()
    expected = bytearray()
    for index in range(frames * channels):
        number = (index * 5 % 15) - 7
        value = number / 8
        if tag == 3:
            payload.extend(struct.pack("<f" if bits == 32 else "<d", value))
        elif bits == 8:
            payload.append(128 + number * 16)
        else:
            payload.extend((number * (1 << (bits - 4))).to_bytes(width, "little", signed=True))
        expected.extend(struct.pack("<f", value))
    body = b"WAVE" + chunk(b"fmt ", fmt)
    if tag == 3:
        body += chunk(b"fact", struct.pack("<I", frames))
    if layout == "junk":
        body += chunk(b"JUNK", b"12345")
    body += chunk(b"data", payload)
    if layout == "trailing":
        body += chunk(b"LIST", b"INFO" + chunk(b"ICMT", b"checked\0"))
    return chunk(b"RIFF", body), expected


def main(dll, dependencies):
    with os.add_dll_directory(dependencies):
        lib = ctypes.CDLL(dll)
    run = lib.AveMediaBridge_ImportAudioToSession
    run.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
    with tempfile.TemporaryDirectory() as temp:
        base = Path(temp)
        for tag, bits in [(1, 8), (1, 16), (1, 24), (1, 32), (3, 32), (3, 64)]:
            for channels in (1, 2):
                for frames in (1, 127, 128, 129, 8193):
                    for layout in ("ordinary", "extensible", "junk", "trailing"):
                        name = f"{tag}-{bits}-{channels}-{frames}-{layout}"
                        path = base / (name + ".wav")
                        path_bytes, expected = fixture(tag, bits, channels, frames, layout)
                        path.write_bytes(path_bytes)
                        session = base / name
                        assert run(str(path), str(session)) == 0, name
                        actual = (session / "original_f32.bin").read_bytes()
                        assert actual == expected, (name, len(expected), len(actual))
                        meta = json.loads((session / "metadata.json").read_text(encoding="utf-8-sig"))
                        budget = meta["presentationBudget"]
                        assert budget["packetScanExecuted"] == (layout == "trailing"), (name, budget)
                        assert budget["known"] and budget["frames"] == frames, (name, budget)

        wave, _ = fixture(3, 32, 2, 8193, "ordinary")
        fmt = wave.index(b"fmt ")
        fact = wave.index(b"fact")
        data = wave.index(b"data")
        def replaced(offset, signature):
            return wave[:offset] + signature + wave[offset + len(signature):]

        rf64_body = b"WAVE" + chunk(b"ds64", bytes(28)) + wave[12:data + 4] + bytes([255]) * 4 + wave[data + 8:]
        rf64 = bytearray(b"RF64" + bytes([255]) * 4 + rf64_body)
        struct.pack_into("<QQQI", rf64, 20, len(rf64) - 8, len(wave) - data - 8, 8193, 0)
        # Inputs outside the structural proof can still open under the ordinary policy.
        # Any accepted input must have gone through the original packet check.
        controls = {
            "incorrect-riff-size": replaced(4, struct.pack("<I", len(wave) - 12)),
            "incorrect-average-byte-rate": replaced(fmt + 16, struct.pack("<I", 8)),
            "incorrect-block-alignment": replaced(fmt + 20, struct.pack("<H", 4)),
            "inconsistent-fact": replaced(fact + 8, struct.pack("<I", 4097)),
            "short-source": wave[:-8],
            "unknown-before-data": chunk(b"RIFF", wave[8:data] + chunk(b"abcd", b"12") + wave[data:]),
            "second-data": chunk(b"RIFF", wave[8:] + chunk(b"data", bytes(8))),
            "unbounded-header": chunk(b"RIFF", wave[8:data] + chunk(b"JUNK", bytes(8192)) + wave[data:]),
            "rf64": rf64,
        }
        for name, content in controls.items():
            path = base / (name + ".wav")
            path.write_bytes(content)
            session = base / name
            if run(str(path), str(session)) == 0:
                meta = json.loads((session / "metadata.json").read_text(encoding="utf-8-sig"))
                assert meta["presentationBudget"]["packetScanExecuted"], (name, meta["presentationBudget"])


if __name__ == "__main__":
    main(*sys.argv[1:])

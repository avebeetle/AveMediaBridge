"""Check incomplete audio with and without an explicit decoder error."""
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def last_aac_packet(file_bytes):
    # This fixture is a single unfragmented, unpadded FFmpeg audio track.
    # All samples are consecutive in one mdat. Parse its actual last sample size.
    def children(data):
        offset = 0
        while offset < len(data):
            size, kind = struct.unpack_from(">I4s", data, offset)
            assert size >= 8 and offset + size <= len(data)
            yield kind, data[offset + 8:offset + size], offset + 8
            offset += size

    def child(data, kind):
        return next(payload for tag, payload, _ in children(data) if tag == kind)

    table = file_bytes
    for kind in (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stsz"):
        table = child(table, kind)
    fixed_size, samples = struct.unpack_from(">II", table, 4)
    assert fixed_size == 0 and samples >= 2
    sizes = struct.unpack_from(">" + "I" * samples, table, 12)
    mdat, start = next((payload, pos) for tag, payload, pos in children(file_bytes) if tag == b"mdat")
    assert sum(sizes) == len(mdat) and sizes[-1] > 0
    return start + len(mdat) - sizes[-1], sizes[-1]


def main(bridge_path, libs, ffmpeg):
    with os.add_dll_directory(str(Path(libs).resolve())), tempfile.TemporaryDirectory() as temporary:
        bridge = ctypes.WinDLL(str(Path(bridge_path).resolve()))
        load = bridge.AveMediaBridge_ImportAudioToSession
        load.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
        error = bridge.AveMediaBridge_GetLastErrorText
        error.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
        root = Path(temporary)
        source = root / "clean.m4a"
        session = root / "session"
        subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                        "sine=frequency=881:sample_rate=48000", "-af", "atrim=end_sample=1025",
                        "-c:a", "aac", "-movie_timescale", "48000", str(source)], check=True)
        data = bytearray(source.read_bytes())
        packet_offset, size = last_aac_packet(data)
        data[packet_offset:packet_offset + size] = bytes(size)
        damaged = root / "last-packet-lost.m4a"
        damaged.write_bytes(data)

        def expect_incomplete(path):
            rc = load(str(path), str(session))
            message = ctypes.create_unicode_buffer(2048)
            error(message, len(message))
            assert rc != 0, f"{path.name} silently published 1024 of 1025 frames"
            assert "incomplete" in message.value.lower(), message.value
            assert "1025" in message.value and "1024" in message.value, message.value
            assert not list(session.glob("*")), "partial artifacts survived failed import"

        expect_incomplete(damaged)

        # An intact FLAC packet can exhaust cleanly even if STREAMINFO promises
        # one more sample. A skipped-packet counter alone would miss this case.
        flac = root / "shorter-than-declared.flac"
        subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                        "sine=frequency=881:sample_rate=48000", "-af", "atrim=end_sample=1024",
                        "-c:a", "flac", str(flac)], check=True)
        flac_data = bytearray(flac.read_bytes())
        assert flac_data[:4] == b"fLaC" and flac_data[4] & 0x7f == 0
        packed_total = struct.unpack_from(">Q", flac_data, 18)[0]
        assert packed_total & ((1 << 36) - 1) == 1024
        struct.pack_into(">Q", flac_data, 18, packed_total + 1)
        flac.write_bytes(flac_data)
        expect_incomplete(flac)

        assert load(str(source), str(session)) == 0, "a healthy retry must succeed"
        info = json.loads((session / "audio_info.json").read_text(encoding="utf-8-sig"))
        assert info["frames"] == 1025 and (session / "original_f32.bin").stat().st_size == 1025 * 4
    print("Incomplete AAC and FLAC rejected. A healthy retry imported all 1025 frames.")


if __name__ == "__main__":
    main(*sys.argv[1:])

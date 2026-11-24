"""Check presentation against decoded packets, including the final audio packet."""
from array import array
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def crc16(block):
    result = 0
    for value in block:
        result ^= value << 8
        for _ in range(8):
            result = ((result << 1) ^ (0x8005 if result & 0x8000 else 0)) & 0xffff
    return result


def main(dll_path, ffmpeg_dir, ffmpeg):
    with os.add_dll_directory(ffmpeg_dir):
        lib = ct.CDLL(dll_path)
    inspect = lib.AveMediaBridge_InspectVobAc3SourceV1
    cancel_fn = ct.CFUNCTYPE(ct.c_int, ct.c_void_p)
    u8, u64, i32 = ct.c_uint8, ct.c_uint64, ct.c_int32
    inspect.argtypes = [ct.c_wchar_p, ct.c_void_p, cancel_fn, ct.c_void_p,
                        ct.POINTER(u8), ct.POINTER(u64), ct.POINTER(u64),
                        ct.POINTER(i32), ct.POINTER(i32), ct.POINTER(i32)]
    probe = lib.AveMediaBridge_ProbeToJson
    probe.argtypes = [ct.c_wchar_p, ct.c_wchar_p]
    import_pcm = lib.AveMediaBridge_ImportAudioToSession
    import_pcm.argtypes = [ct.c_wchar_p, ct.c_wchar_p]
    win = ct.WinDLL("kernel32", use_last_error=True)
    win.CreateFileW.argtypes = [ct.c_wchar_p, ct.c_uint32, ct.c_uint32, ct.c_void_p,
                                ct.c_uint32, ct.c_uint32, ct.c_void_p]
    win.CreateFileW.restype = ct.c_void_p
    win.CloseHandle.argtypes = [ct.c_void_p]

    def packet_count(path, expected_status, expected_frames=None, cancel_after=None):
        pin = win.CreateFileW(str(path), 0x80000000, 1, None, 3, 0, None)
        assert pin != ct.c_void_p(-1).value
        try:
            sha, size, frames = (u8 * 32)(*([121] * 32)), u64(19), u64(19)
            index, hz, ch = i32(-1), i32(-1), i32(-1)
            polls = 0
            def canceled(_):
                nonlocal polls
                polls += 1
                return cancel_after is not None and polls >= cancel_after
            status = inspect(str(path), pin, cancel_fn(canceled), None, sha, ct.byref(size),
                             ct.byref(frames), ct.byref(index), ct.byref(hz), ct.byref(ch))
            assert status == expected_status, (path, status)
            if status in (1, 2):
                b = path.read_bytes()
                assert bytes(sha) == hashlib.sha256(b).digest() and size.value == len(b)
            else:
                assert bytes(sha) == bytes([121]) * 32 and size.value == 19
            if status == 1:
                assert frames.value == expected_frames and index.value == 1 and hz.value == 48000 and ch.value == 2
            else:
                assert (frames.value, index.value, hz.value, ch.value) == (19, -1, -1, -1)
        finally:
            win.CloseHandle(pin)

    def encode(path, n, double=False):
        cmd = [ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
               "color=c=black:s=176x144:r=25:d=0.3", "-f", "lavfi", "-i",
               "sine=frequency=450:sample_rate=" + ("48000" if path.suffix == ".vob" else "44100"),
               "-map", "0:v", "-map", "1:a"]
        if double:
            cmd += ["-map", "1:a"]
        cmd += ["-af", f"atrim=end_sample={n}", "-ac", "2",
                "-c:v", "mpeg2video" if path.suffix == ".vob" else "flv",
                "-threads:v", "1", "-c:a", "ac3" if path.suffix == ".vob" else "libmp3lame",
                "-b:a", "192k", str(path)]
        subprocess.run(cmd, check=True)
        audio = subprocess.check_output([ffmpeg, "-nostdin", "-loglevel", "error", "-i", str(path),
                                         "-map", "0:a:0", "-f", "f32le", "-"])
        return array("f", audio)

    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        for ext, lengths in [("vob", [1535, 1536, 1537, 48017]), ("flv", [1151, 1152, 1153, 88217])]:
            for n in lengths:
                f = d / f"{n}.{ext}"
                reference = encode(f, n)
                decoded = len(reference) // 2
                assert decoded > 0
                if ext == "vob":
                    packet_count(f, 1, decoded)
                else:
                    j = d / "probe.json"
                    assert probe(str(f), str(j)) == 0
                    meta = json.loads(j.read_text(encoding="utf-8-sig"))
                    assert meta["decodedSampleFramesSource"] == "flv_mp3_packet_presentation"
                    assert meta["decodedSampleFrames"] == decoded
                session = d / (f.name + ".session")
                assert import_pcm(str(f), str(session)) == 0, f.name
                actual = array("f", (session / "original_f32.bin").read_bytes())
                assert len(actual) == len(reference), f.name
                assert max(abs(a - b) for a, b in zip(actual, reference)) < 1e-5, f.name
        good = d / "48017.vob"
        packet_count(good, -1, cancel_after=1)
        packet_count(good, -1, cancel_after=4)
        changed = d / "changed.vob"
        data = bytearray(good.read_bytes())
        start = data.index(b"\x00\x00\x01\xbd")
        offset = start + 9 + data[start + 8] + 4
        assert data[offset:offset + 2] == b"\x0b\x77"
        data[offset + 20] ^= 1
        changed.write_bytes(data)
        packet_count(changed, 2)
        # Damage both independent AC-3 CRC regions while keeping the combined CRC zero.
        frame_bytes = 768  # 192 kb/s, 48 kHz, 1536 samples
        assert int.from_bytes(data[start + 4:start + 6], "big") + start + 6 >= offset + frame_bytes
        assert crc16(data[offset + 2:offset + frame_bytes]) != 0
        correction = crc16(data[offset + 2:offset + frame_bytes - 2])
        data[offset + frame_bytes - 2:offset + frame_bytes] = correction.to_bytes(2, "big")
        assert crc16(data[offset + 2:offset + frame_bytes]) == 0
        changed.write_bytes(data)
        packet_count(changed, 2)
        changed.write_bytes(data[:-1])
        packet_count(changed, 0)
        data = bytearray(good.read_bytes())
        data[start + 7] |= 0x40  # DTS declared without adding its five bytes
        changed.write_bytes(data)
        packet_count(changed, 2)
        multi = d / "multi.vob"
        encode(multi, 48017, double=True)
        packet_count(multi, 2)


if __name__ == "__main__":
    main(*sys.argv[1:])

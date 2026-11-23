"""Qualified CAF lengths, pinned-file identity, fallback and cancellation."""
import ctypes as ct
import hashlib
import msvcrt
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def chunks(path):
    raw = path.read_bytes()
    assert raw[:8] == b"caff\0\1\0\0"
    result = {}
    start = 8
    while start < len(raw):
        name, size = struct.unpack_from(">4sQ", raw, start)
        start += 12
        assert size <= len(raw) - start and name not in result
        result[name] = raw[start:start + size]
        start += size
    return result


def write_caf(path, sections):
    with path.open("wb") as out:
        out.write(b"caff\0\1\0\0")
        for name, payload in sections.items():
            out.write(struct.pack(">4sQ", name, len(payload)))
            out.write(payload)


def packet_field(number):
    result = [number & 127]
    number >>= 7
    while number:
        result.append(128 | (number & 127))
        number >>= 7
    return bytes(reversed(result))


def main(dll_path, ffmpeg_dir, ffmpeg):
    with os.add_dll_directory(ffmpeg_dir):
        dll = ct.CDLL(dll_path)
    cancel_type = ct.CFUNCTYPE(ct.c_int, ct.c_void_p)
    u8, u64, i32 = ct.c_uint8, ct.c_uint64, ct.c_int32
    inspect = dll.AveMediaBridge_InspectCafAlacSourceV1
    inspect.argtypes = [ct.c_wchar_p, ct.c_void_p, cancel_type, ct.c_void_p, ct.POINTER(u8),
                        ct.POINTER(u64), ct.POINTER(u64), ct.POINTER(i32), ct.POINTER(i32), ct.POINTER(i32)]
    inspect.restype = ct.c_int
    create = ct.WinDLL("kernel32", use_last_error=True).CreateFileW
    create.argtypes = [ct.c_wchar_p, ct.c_uint32, ct.c_uint32, ct.c_void_p, ct.c_uint32,
                       ct.c_uint32, ct.c_void_p]
    create.restype = ct.c_void_p

    def check(path, status, frames=None, channels=2, cancel_after=None):
        pin = create(str(path), 0x80000000, 1, None, 3, 0, None)
        assert pin != ct.c_void_p(-1).value, ct.get_last_error()
        polls = 0

        def cancel(_):
            nonlocal polls
            polls += 1
            return int(cancel_after is not None and polls >= cancel_after)

        sentinel = 0xcd
        digest = (u8 * 32)(*([sentinel] * 32))
        values = [typ(sentinel) for typ in (u64, u64, i32, i32, i32)]
        with os.fdopen(msvcrt.open_osfhandle(pin, os.O_RDONLY | os.O_BINARY), "rb", buffering=0) as source:
            source.seek(6)
            actual = inspect(str(path.with_name("display name only.caf")), pin, cancel_type(cancel), None,
                             digest, *(ct.byref(value) for value in values))
            assert source.tell() == 6
        assert actual == status, (path, actual, status)
        expected = [sentinel] * 5
        expected_digest = bytes([sentinel]) * 32
        if status in (1, 2):
            expected_digest = hashlib.sha256(path.read_bytes()).digest()
            expected[0] = path.stat().st_size
        if status == 1:
            expected[1:] = [frames, 0, 44100, channels]
        assert [v.value for v in values] == expected and bytes(digest) == expected_digest

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        packets = {}
        for channels in (1, 2):
            for length in (1, 7, 4095, 4096, 4097, 8192, 8193):
                dest = root / f"{channels}-{length}.caf"
                pcm = b"".join(struct.pack("<h", (i * 257 % 60001) - 30000)
                               for i in range(length * channels))
                subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-f", "s16le", "-ar", "44100",
                                "-ac", str(channels), "-i", "-", "-c:a", "alac", str(dest)],
                               input=pcm, check=True)
                check(dest, 1, length, channels)
                if channels == 2 and length in (1, 7, 4096):
                    packets[length] = chunks(dest)[b"data"][4:]

        base = chunks(root / "2-4096.caf")
        counts = (4096, 1, 4096, 7)
        joined = base.copy()
        joined[b"data"] = bytes(4) + b"".join(packets[n] for n in counts)
        joined[b"pakt"] = struct.pack(">QQII", 4, 4 * 4096, 0, 0) + b"".join(
            packet_field(len(packets[n])) for n in counts)
        path = root / "interior.caf"
        write_caf(path, joined)
        check(path, 1, sum(counts))
        check(path, -1, cancel_after=1)

        variable = joined.copy()
        desc = bytearray(variable[b"desc"])
        struct.pack_into(">I", desc, 20, 0)
        variable[b"desc"] = desc
        table = b"".join(packet_field(len(packets[n])) + packet_field(n) for n in counts)
        variable[b"pakt"] = struct.pack(">QQII", 4, sum(counts), 0, 0) + table
        write_caf(path, variable)
        check(path, 1, sum(counts))
        variable[b"pakt"] = struct.pack(">QQII", 4, sum(counts) - 1, 1, 0) + table
        write_caf(path, variable)
        check(path, 0)

        # A playable nonzero ALAC element tag is outside the qualified layout.
        edited = bytearray(joined[b"data"])
        edited[4] |= 2
        joined[b"data"] = edited
        write_caf(path, joined)
        check(path, 2)

        # A seven-byte explicit-count header may straddle a SHA input block.
        tiny = chunks(root / "2-7.caf")
        before_data = {}
        for tag, body in tiny.items():
            if tag == b"data":
                break
            before_data[tag] = body
        offset = 8 + sum(12 + len(body) for body in before_data.values()) + 12 + 4
        for in_first_block in (1, 6):
            padding = (1 << 20) - offset - 12 - in_first_block
            write_caf(path, {**before_data, b"free": bytes(padding), **tiny})
            check(path, 1, 7)


if __name__ == "__main__":
    main(*sys.argv[1:])

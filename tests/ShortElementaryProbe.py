"""Exercise weak elementary-stream probes in the supplied runtime and import the complete PCM."""
import argparse
import array
import ctypes as ct
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import wave


class ImportOptions(ct.Structure):
    _fields_ = [("size", ct.c_uint32), ("source", ct.c_wchar_p), ("output", ct.c_wchar_p),
                ("progress", ct.c_void_p), ("cancel", ct.c_void_p), ("ctx", ct.c_void_p),
                ("waveform", ct.c_void_p)]


class ProbeData(ct.Structure):
    _fields_ = [("filename", ct.c_char_p), ("buf", ct.c_void_p),
                ("size", ct.c_int), ("mime", ct.c_char_p)]


def write_wav(path, rate, frames):
    data = array.array("h")
    for i in range(frames):
        for c in (0, 1):
            data.append(int(13500 * math.sin((i + 2) * (.113 + c * .013)) +
                            5200 * math.cos((i + 5) * (.022 + c * .008))))
    with wave.open(str(path), "wb") as f:
        f.setparams((2, 2, rate, 0, "NONE", ""))
        f.writeframes(data.tobytes())


def main(bridge, ffmpeg_dir, ffmpeg):
    def ff(*args):
        return subprocess.check_output([str(ffmpeg), "-nostdin", "-y", "-loglevel", "error", *map(str, args)])

    available = {fields[1] for line in ff("-encoders").decode(errors="replace").splitlines()
                 if len(fields := line.split()) >= 2}
    required = {"aac", "ac3", "eac3", "libmp3lame"}
    if required - available:
        print("Missing test encoders:", ", ".join(sorted(required - available)))
        return 77

    with os.add_dll_directory(str(ffmpeg_dir)), tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        av = ct.CDLL(str(ffmpeg_dir / "avformat-61.dll"))
        av.av_probe_input_format3.argtypes = [ct.POINTER(ProbeData), ct.c_int, ct.POINTER(ct.c_int)]
        av.av_probe_input_format3.restype = ct.POINTER(ct.c_char_p)
        abi = ct.WinDLL(str(bridge))
        abi.AveMediaBridge_ProbeToJson.argtypes = [ct.c_wchar_p, ct.c_wchar_p]
        abi.AveMediaBridge_ImportAudioToSessionGuardedV1.argtypes = [ct.POINTER(ImportOptions)]

        def probe(data):
            buf = ct.create_string_buffer(data, len(data) + 32)
            pd = ProbeData(b"", ct.cast(buf, ct.c_void_p), len(data), None)
            score = ct.c_int()
            fmt = av.av_probe_input_format3(ct.byref(pd), 1, ct.byref(score))
            return (fmt[0].decode() if fmt else None), score.value

        def check_pcm(source, reference, expected_frames):
            with tempfile.TemporaryDirectory(dir=root) as temp:
                target = Path(temp)
                info = target / "probe.json"
                assert abi.AveMediaBridge_ProbeToJson(str(source), str(info)) == 0, source.name
                parsed = json.loads(info.read_text(encoding="utf-8-sig"))
                assert parsed["channels"] == 2 and parsed["codecName"] in ("aac", "ac3", "eac3")
                options = ImportOptions(ct.sizeof(ImportOptions), str(source), temp, None, None, None, None)
                assert abi.AveMediaBridge_ImportAudioToSessionGuardedV1(ct.byref(options)) == 0, source.name
                audio = (target / "original_f32.bin").read_bytes()
                assert len(audio) == len(reference) == expected_frames * 8, source.name
                a, b = array.array("f", audio), array.array("f", reference)
                assert max(abs(x - y) for x, y in zip(a, b)) < 1e-5, source.name

        cases = []
        for n in (1, 129):
            cases.append(("aac", "adts", "aac", n, 44100, 2048))
        for codec, ext in (("ac3", "ac3"), ("eac3", "eac3"), ("eac3", "ec3")):
            for n in (3071, 3072, 3073):
                cases.append((codec, codec, ext, n, 48000, 4608 if n == 3073 else 3072))
        samples = {}
        for codec, mux, ext, n, rate, expected in cases:
            wav = root / "in.wav"
            write_wav(wav, rate, n)
            source = root / f"{n}.{ext}"
            ff("-i", wav, "-c:a", codec, "-threads:a", "1", "-b:a",
               "128k" if codec == "aac" else "192k", "-f", mux, source)
            data = source.read_bytes()
            fmt, score = probe(data)
            assert fmt == codec, (source.name, fmt, score)
            reference = ff("-f", codec, "-i", source, "-map", "0:a:0", "-f", "f32le", "-")
            check_pcm(source, reference, expected)
            unknown = root / "unknown.bin"
            shutil.copyfile(source, unknown)
            check_pcm(unknown, reference, expected)
            samples.setdefault(codec, data)

        # The tie-break must require complete frames from the beginning of the probe window.
        for codec, data in samples.items():
            for invalid in (data[:-1], data + b"\x00", b"\x01" + data):
                assert probe(invalid) != (codec, 2), codec
        # No new confidence for a second ADTS frame with a different sample rate.
        aac = bytearray(samples["aac"])
        second = ((aac[3] & 3) << 11) | (aac[4] << 3) | (aac[5] >> 5)
        aac[second + 2] ^= 0x04
        assert probe(bytes(aac)) != ("aac", 2)
        # Both ADTS headers still agree when an entire payload is corrupt.
        # Never commit just the half of the recording that happened to decode.
        abi.AveMediaBridge_ImportAudioToSession.argtypes = [ct.c_wchar_p, ct.c_wchar_p]
        for begin, end in ((7, second), (second + 7, len(aac))):
            broken = bytearray(samples["aac"])
            broken[begin:end] = bytes(end - begin)
            damaged = root / "damaged.aac"
            damaged.write_bytes(broken)
            for guarded in (True, False):
                with tempfile.TemporaryDirectory(dir=root) as temp:
                    if guarded:
                        opts = ImportOptions(ct.sizeof(ImportOptions), str(damaged), temp, None, None, None, None)
                        rc = abi.AveMediaBridge_ImportAudioToSessionGuardedV1(ct.byref(opts))
                    else:
                        rc = abi.AveMediaBridge_ImportAudioToSession(str(damaged), temp)
                    assert rc != 0, (begin, guarded, "partial import accepted")
                    assert not (Path(temp) / "original_f32.bin").exists()
        # A whole probe read can end between the second and third frames of a larger file.
        # Score 2 must not pass libavformat's normal retry threshold (25).
        assert probe(samples["aac"]) == ("aac", 2)
        assert probe(samples["aac"] + bytes(8192)) != ("aac", 2)
        # A CRC mismatch in the second AC-3 or E-AC-3 frame must still veto the tie-break.
        for codec in ("ac3", "eac3"):
            invalid = bytearray(samples[codec])
            invalid[-1] ^= 1
            assert probe(bytes(invalid)) != (codec, 2)
        # A transition between Dolby codec families is not a fully consistent raw stream.
        split = len(samples["ac3"]) // 2
        mixed = samples["ac3"][:split] + samples["eac3"][split:]
        assert probe(mixed) not in (("ac3", 2), ("eac3", 2))
        # Payloads containing a sync word must not displace ordinary MP3 or its container.
        write_wav(root / "mp3-in.wav", 44100, 44100)
        for mux, suffix in (("mp3", "mp3"), ("mp4", "mp4")):
            source = root / ("control." + suffix)
            ff("-i", root / "mp3-in.wav", "-c:a", "libmp3lame", "-b:a", "128k", "-f", mux, source)
            assert mux in probe(source.read_bytes())[0].split(","), source.name
            misleading = root / "mp3-data.aac"
            shutil.copyfile(source, misleading)
            info = root / "control-probe.json"
            assert abi.AveMediaBridge_ProbeToJson(str(misleading), str(info)) == 0
            assert json.loads(info.read_text(encoding="utf-8-sig"))["codecName"] == "mp3"
        assert probe((root / "mp3-in.wav").read_bytes())[0] == "wav"
        print("11 generated streams imported with their original and unknown extensions; "
              "full PCM, weak probe negatives and neighbouring formats passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("bridge", type=Path)
    parser.add_argument("ffmpeg_dir", type=Path)
    parser.add_argument("ffmpeg", type=Path)
    args = parser.parse_args()
    raise SystemExit(main(args.bridge.resolve(), args.ffmpeg_dir.resolve(), args.ffmpeg.resolve()))

"""Exercise independent guarded imports in one DLL while PCM callbacks overlap."""
from concurrent.futures import ThreadPoolExecutor
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from threading import Event

from IncompleteAudioImport import last_aac_packet
from MatroskaVorbisTrack import Options, ProgressCallback


INDEX = "mp4_aac_sample_edit_table_presentation"
CancelCallback = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p)


class Gate:
    def __init__(self, threshold):
        self.threshold = threshold
        self.reached = Event()
        self.release = Event()
        self.cancel = Event()
        self.frames = None
        self.timed_out = False

    def observe(self, frames):
        if frames >= self.threshold and self.frames is None:
            self.frames = frames
            self.reached.set()
            self.timed_out = not self.release.wait(20)


def exercise(library, libs, ffmpeg):
    with os.add_dll_directory(str(Path(libs).resolve())), tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        bridge = ctypes.CDLL(str(Path(library).resolve()))
        guarded = bridge.AveMediaBridge_ImportAudioToSessionGuardedV1
        guarded.argtypes = [ctypes.POINTER(Options)]
        last_error = bridge.AveMediaBridge_GetLastErrorText
        last_error.argtypes = [ctypes.c_wchar_p, ctypes.c_int]

        def encode(name, frames, rate, channels, scale, frequency):
            path = root / name
            subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                            f"sine=frequency={frequency}:sample_rate={rate}", "-af", f"atrim=end_sample={frames}",
                            "-ac", str(channels), "-c:a", "aac", "-movie_timescale", str(scale), str(path)],
                           check=True)
            return path

        def run(media, session, gate=None, cancel_at_start=False):
            frames_seen = []

            def progress(state, _):
                frames = state.contents.written_frames
                frames_seen.append(frames)
                if gate is not None and not (state.contents.flags & 1):
                    gate.observe(frames)

            cancel = CancelCallback(lambda _: int(cancel_at_start or (gate is not None and gate.cancel.is_set())))
            options = Options(ctypes.sizeof(Options), str(media), str(session), ProgressCallback(progress),
                              ctypes.cast(cancel, ctypes.c_void_p))
            code = guarded(ctypes.byref(options))
            error = ctypes.create_unicode_buffer(2048)
            last_error(error, len(error))
            assert frames_seen == sorted(frames_seen), (media.name, frames_seen)
            if code:
                assert not list(session.glob("*")), (media.name, code, list(session.glob("*")))
                if cancel_at_start:
                    assert code == 4 and not any(frames_seen), (media.name, code, frames_seen)
                return code, error.value
            info = json.loads((session / "audio_info.json").read_bytes())
            budget = json.loads((session / "metadata.json").read_bytes())["presentationBudget"]
            pcm = (session / "original_f32.bin").read_bytes()
            assert frames_seen[-1] == info["frames"] and len(pcm) == info["frames"] * info["channels"] * 4
            return code, (info["frames"], budget["source"], hashlib.sha256(pcm).hexdigest())

        a = encode("indexed.mp4", 262145, 48000, 2, 48000, 331)
        b = encode("rounded.m4a", 441011, 44100, 1, 1000, 557)
        short = encode("short.m4a", 1025, 48000, 1, 48000, 881)
        data = bytearray(short.read_bytes())
        offset, size = last_aac_packet(data)
        data[offset:offset + size] = bytes(size)
        damaged = root / "damaged.m4a"
        damaged.write_bytes(data)

        expected = {p: run(p, root / (p.stem + "-solo")) for p in (a, b, short)}
        assert all(rc == 0 for rc, _ in expected.values()), expected
        assert expected[a][1][:2] == (262145, INDEX) and expected[short][1][:2] == (1025, INDEX)
        assert expected[b][1][1] != INDEX and len({value[1][2] for value in expected.values()}) == 3

        for label, threshold in (("early", 1), ("late", 225000)):
            blocked = Gate(threshold)
            survivor = Gate(1)
            canceled_dir = root / (label + "-canceled")
            retry_dir = root / (label + "-retry")
            with ThreadPoolExecutor(max_workers=3) as pool:
                first = pool.submit(run, a, canceled_dir, blocked)
                second = pool.submit(run, b, root / (label + "-survivor"), survivor)
                try:
                    assert blocked.reached.wait(10) and survivor.reached.wait(10), "imports never overlapped"
                    code, message = pool.submit(run, a, retry_dir, cancel_at_start=True).result(timeout=10)
                    assert code == 4 and "cancel" in message.lower(), (code, message)
                    code, message = pool.submit(run, damaged, retry_dir).result(timeout=10)
                    assert code == 2 and "incomplete" in message.lower(), (code, message)
                    assert "1025" in message and "1024" in message, message
                    assert pool.submit(run, short, retry_dir).result(timeout=10) == expected[short]
                    if label == "early":
                        survivor.release.set()
                        assert second.result(timeout=10) == expected[b] and not first.done()
                    blocked.cancel.set()
                    blocked.release.set()
                    code, message = first.result(timeout=10)
                    assert code == 4 and "cancel" in message.lower(), (code, message)
                    assert pool.submit(run, a, canceled_dir).result(timeout=10) == expected[a]
                    assert label == "early" or not second.done()
                    survivor.release.set()
                    assert second.result(timeout=10) == expected[b]
                    assert not blocked.timed_out and not survivor.timed_out
                finally:
                    blocked.release.set()
                    survivor.release.set()
            print(f"{label}: canceled at {blocked.frames}/{expected[a][1][0]} frames; "
                  f"survivor {expected[b][1][0]} frames; concurrent cancel before PCM and corrupt input "
                  f"both cleaned up; both retries matched")


if __name__ == "__main__":
    exercise(*sys.argv[1:])

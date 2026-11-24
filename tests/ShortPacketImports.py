"""Compare short Opus presentations across containers and ALAC against its PCM input."""
from array import array
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import wave


def make_input(root, frames):
    path = root / f'{frames}.wav'
    values = array('h', ((i * 1103 + 5161) % 24001 - 12000 for i in range(frames * 2)))
    with wave.open(str(path), 'wb') as writer:
        writer.setparams((2, 2, 48000, 0, 'NONE', ''))
        writer.writeframes(values.tobytes())
    return path, array('f', (v / 32768 for v in values)).tobytes()


def main(bridge_path, runtime, ffmpeg):
    encoders = subprocess.check_output([ffmpeg, '-v', 'error', '-encoders']).decode(errors='replace')
    available = {fields[1] for line in encoders.splitlines() if len(fields := line.split()) >= 2}
    if not {'libopus', 'mpeg4', 'alac'}.issubset(available):
        print('The fixture generator requires libopus, mpeg4 and alac.')
        return 77
    with os.add_dll_directory(str(Path(runtime).resolve())):
        dll = ctypes.WinDLL(str(Path(bridge_path).resolve()))
        import_pcm = dll.AveMediaBridge_ImportAudioToSession
        import_pcm.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
        probe = dll.AveMediaBridge_ProbeToJson
        probe.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]

        def encode(*args):
            subprocess.run([ffmpeg, '-nostdin', '-v', 'error', '-y', *map(str, args)], check=True)

        with tempfile.TemporaryDirectory() as work:
            root = Path(work)

            def read(path, frames):
                session = root / (path.name + '.session')
                info = root / 'probe.json'
                assert probe(str(path), str(info)) == 0, path.name
                facts = json.loads(info.read_text(encoding='utf-8-sig'))
                assert facts['decodedSampleFrames'] == frames, (path.name, facts['decodedSampleFrames'], frames)
                status = import_pcm(str(path), str(session))
                assert status == 0, (path.name, status)
                pcm = (session / 'original_f32.bin').read_bytes()
                assert len(pcm) == frames * 8, (path.name, len(pcm) // 8, frames)
                return pcm

            for frames in (1, 127, 647, 648, 649, 1919, 8201):
                wav, _ = make_input(root, frames)
                ogg = wav.with_suffix('.opus')
                encode('-i', wav, '-c:a', 'libopus', '-application', 'audio',
                       '-frame_duration', '20', ogg)
                expected = read(ogg, frames)
                for ext in ('mka', 'webm'):
                    file = wav.with_suffix('.' + ext)
                    encode('-i', ogg, '-map', '0:a:0', '-c:a', 'copy', file)
                    assert read(file, frames) == expected, file.name

            for frames in (1, 2, 4095, 4096, 4097):
                wav, expected = make_input(root, frames)
                mov = wav.with_suffix('.mov')
                encode('-i', wav, '-f', 'lavfi', '-i', 'color=s=32x32:r=25:d=0.12',
                       '-map', '1:v', '-map', '0:a', '-c:v', 'mpeg4', '-c:a', 'alac',
                       '-movie_timescale', '48000', mov)
                assert read(mov, frames) == expected, mov.name


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))

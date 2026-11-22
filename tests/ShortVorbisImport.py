"""Check the whole decoded signal against the same packets on separate Ogg pages."""
from array import array
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def vorbis_packets(path):
    data = path.read_bytes()
    pos = 0
    serial = None
    pending = bytearray()
    packets = []
    audio_pages = 0
    while pos < len(data):
        assert data[pos:pos + 5] == b'OggS\0'
        segments = data[pos + 26]
        payload = pos + 27 + segments
        lacing = data[pos + 27:payload]
        end = payload + sum(lacing)
        assert end <= len(data)
        stream = data[pos + 14:pos + 18]
        if data[pos + 5] & 2 and data[payload:payload + 7] == b'\x01vorbis':
            assert serial is None
            serial = stream
        if stream == serial:
            if len(packets) >= 3:
                audio_pages += 1
            for size in lacing:
                pending.extend(data[payload:payload + size])
                payload += size
                if size < 255:
                    packets.append(bytes(pending))
                    pending.clear()
        pos = end
    assert not pending and len(packets) >= 5
    return (packets[0], packets[2], packets[3:]), audio_pages


def main(bridge_dll, ffmpeg_dir, ffmpeg):
    def ff(*args):
        return subprocess.check_output([ffmpeg, '-nostdin', '-v', 'error', *args])

    encoders = ff('-encoders').decode(errors='replace').splitlines()
    available = {parts[1] for line in encoders if len(parts := line.split()) >= 2}
    if not {'libvorbis', 'libtheora'}.issubset(available):
        print('The fixture generator needs libvorbis and libtheora encoders.')
        return 77

    with os.add_dll_directory(ffmpeg_dir), tempfile.TemporaryDirectory() as temp:
        dll = ctypes.WinDLL(bridge_dll)
        import_audio = dll.AveMediaBridge_ImportAudioToSession
        import_audio.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p)
        import_audio.restype = ctypes.c_int
        root = Path(temp)
        for length in (128, 1023, 1024, 1025, 2048):
            audio = ('-f', 'lavfi', '-i', 'sine=frequency=673:sample_rate=44100')
            encode = ('-af', f'atrim=end_sample={length}', '-c:a', 'libvorbis', '-q:a', '4')
            control = root / f'control-{length}.ogg'
            ff(*audio, *encode, '-page_duration', '1', str(control))
            packets, pages = vorbis_packets(control)
            assert pages >= 2
            reference = array('f', ff('-i', str(control), '-map', '0:a:0', '-f', 'f32le', 'pipe:1'))
            assert len(reference) == length and max(map(abs, reference[-64:])) > 0.01
            for ext in ('ogg', 'oga', 'ogv'):
                single = root / f'single-{length}.{ext}'
                video = ('-f', 'lavfi', '-i', 'color=s=64x64:r=25:d=0.12',
                         '-map', '1:v:0', '-map', '0:a:0', '-c:v', 'libtheora') if ext == 'ogv' else ()
                ff(*audio, *video, *encode, '-f', 'ogg', str(single))
                assert vorbis_packets(single) == (packets, 1), f'mismatched fixture: {single.name}'
                session = root / f'import-{length}-{ext}'
                rc = import_audio(str(single), str(session))
                assert rc == 0, f'{single.name}: import returned {rc}'
                pcm = array('f', (session / 'original_f32.bin').read_bytes())
                assert len(pcm) == length, (single.name, len(pcm), length)
                assert max(abs(a-b) for a, b in zip(pcm, reference)) < 1e-5, single.name
                print(f'{single.name}: all {length} frames verified')


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))

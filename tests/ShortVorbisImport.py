"""Check the whole decoded signal against the same packets on separate Ogg pages."""
from array import array
import ctypes
import json
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

        # A terminal granule describes one link, not the entire physical Ogg.
        chain = root / 'two-links.oga'
        lengths = (1023, 2048)
        chain.write_bytes(b''.join((root / f'single-{n}.ogg').read_bytes() for n in lengths))
        evidence = root / 'probe.json'
        probe = dll.AveMediaBridge_ProbeToJson
        probe.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p)
        assert probe(str(chain), str(evidence)) == 0
        facts = json.loads(evidence.read_text(encoding='utf-8-sig'))
        assert facts['decodedSampleFramesKind'] == 'exact'
        assert facts['decodedSampleFrames'] == sum(lengths), facts['decodedSampleFrames']
        result = root / 'chain-import'
        assert import_audio(str(chain), str(result)) == 0
        pcm = (result / 'original_f32.bin').read_bytes()
        expected = b''.join((root / f'import-{n}-ogg' / 'original_f32.bin').read_bytes()
                            for n in lengths)
        assert pcm == expected

        # The first audio page may be EOS, or contain only the priming packet.
        # Both page arrangements must preserve the same six distinct channels.
        tones = '|'.join(f'0.1*sin(2*PI*{hz}*t)' for hz in (431, 587, 643, 53, 839, 1009))
        audio = ('-f', 'lavfi', '-i', f'aevalsrc={tones}:s=48000:c=5.1',
                 '-af', 'atrim=end_sample=1023', '-c:a', 'libvorbis', '-q:a', '4')
        single = root / 'six-single.ogg'
        split = root / 'six-split.ogg'
        ff(*audio, str(single))
        ff(*audio, '-page_duration', '1', str(split))
        single_packets, single_pages = vorbis_packets(single)
        split_packets, split_pages = vorbis_packets(split)
        assert single_pages == 1 and split_pages > 1 and single_packets == split_packets
        reference = array('f', ff('-i', str(split), '-map', '0:a:0', '-f', 'f32le', 'pipe:1'))
        assert len(reference) == 1023 * 6
        outputs = []
        for surround in (single, split):
            assert probe(str(surround), str(evidence)) == 0
            facts = json.loads(evidence.read_text(encoding='utf-8-sig'))
            assert (facts['decodedSampleFrames'], facts['channels'], facts['sampleRate']) == (1023, 6, 48000)
            assert facts['decodedSampleFramesKind'] == 'exact'
            session = root / (surround.stem + '-import')
            assert import_audio(str(surround), str(session)) == 0
            output = (session / 'original_f32.bin').read_bytes()
            samples = array('f', output)
            assert len(samples) == len(reference) and max(abs(a-b) for a,b in zip(samples, reference)) < 1e-5
            outputs.append(output)
        assert outputs[0] == outputs[1]

        changed_layout = root / 'mono-then-six.ogg'
        changed_layout.write_bytes((root / 'single-1023.ogg').read_bytes() + single.read_bytes())
        assert probe(str(changed_layout), str(evidence)) != 0
        facts = json.loads(evidence.read_text(encoding='utf-8-sig'))
        assert facts['mediaOpenReason'] == 'unsupported_channel_layout'
        session = root / 'changed-layout-import'
        assert import_audio(str(changed_layout), str(session)) != 0
        assert not (session / 'original_f32.bin').exists()


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))

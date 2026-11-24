"""Exercise runtime AAC index admission on independently encoded audio of different lengths."""
from array import array
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave


INDEX_SOURCE = 'mp4_aac_sample_edit_table_presentation'


def boxes(data, start, end):
    while start < end:
        size, tag = struct.unpack_from('>I4s', data, start)
        assert 8 <= size <= end - start
        yield tag, start, start + size
        start += size


def child(data, start, end, name):
    return next((a, b) for tag, a, b in boxes(data, start, end) if tag == name)


def audio_sample_ranges(data):
    """Locate each packet of the FFmpeg fixture's single local audio track."""
    moov, moov_end = child(data, 0, len(data), b'moov')
    for tag, track, track_end in boxes(data, moov + 8, moov_end):
        if tag != b'trak':
            continue
        mdia, mdia_end = child(data, track + 8, track_end, b'mdia')
        hdlr, _ = child(data, mdia + 8, mdia_end, b'hdlr')
        if data[hdlr + 16:hdlr + 20] != b'soun':
            continue
        minf, minf_end = child(data, mdia + 8, mdia_end, b'minf')
        stbl, stbl_end = child(data, minf + 8, minf_end, b'stbl')
        stsz, _ = child(data, stbl + 8, stbl_end, b'stsz')
        stco, _ = child(data, stbl + 8, stbl_end, b'stco')
        stsc, _ = child(data, stbl + 8, stbl_end, b'stsc')
        fixed, count = struct.unpack_from('>II', data, stsz + 12)
        assert fixed == 0
        sizes = struct.unpack_from(f'>{count}I', data, stsz + 20)
        offsets_count = struct.unpack_from('>I', data, stco + 12)[0]
        offsets = struct.unpack_from(f'>{offsets_count}I', data, stco + 16)
        group_count = struct.unpack_from('>I', data, stsc + 12)[0]
        groups = [struct.unpack_from('>III', data, stsc + 16 + 12 * i) for i in range(group_count)]
        result = []
        for i, (first, per_chunk, description) in enumerate(groups):
            assert description == 1
            stop = groups[i + 1][0] - 1 if i + 1 < len(groups) else len(offsets)
            for offset in offsets[first - 1:stop]:
                for _ in range(per_chunk):
                    end = offset + sizes[len(result)]
                    result.append((offset, end))
                    offset = end
        assert len(result) == count
        return result
    raise AssertionError('fixture audio table was not found')


def duplicate_access_unit(path):
    """Keep one valid sample table entry but place two AAC access units inside it."""
    data = bytearray(path.read_bytes())

    mdat, mdat_end = child(data, 0, len(data), b'mdat')
    moov, moov_end = child(data, 0, len(data), b'moov')
    assert mdat_end == moov
    start, end = moov, moov_end
    for tag in (b'trak', b'mdia', b'minf', b'stbl'):
        start, end = child(data, start + 8, end, tag)
    stsz, _ = child(data, start + 8, end, b'stsz')
    stco, _ = child(data, start + 8, end, b'stco')
    fixed_size, count = struct.unpack_from('>II', data, stsz + 12)
    chunks, first_byte = struct.unpack_from('>II', data, stco + 12)
    assert fixed_size == 0 and chunks == 1 and count >= 5
    lengths = struct.unpack_from(f'>{count}I', data, stsz + 20)
    assert first_byte == mdat + 8 and first_byte + sum(lengths) == mdat_end
    # Avoid priming and end padding. This also leaves the file's edit list intact.
    index = 3
    size = lengths[index]
    offset = first_byte + sum(lengths[:index])
    struct.pack_into('>I', data, mdat, mdat_end - mdat + size)
    struct.pack_into('>I', data, stsz + 20 + 4 * index, 2 * size)
    data[offset:offset] = data[offset:offset + size]
    return data


def exercise(bridge_path, runtime, ffmpeg):
    with os.add_dll_directory(str(Path(runtime).resolve())), tempfile.TemporaryDirectory() as work:
        root = Path(work)
        dll = ctypes.WinDLL(str(Path(bridge_path).resolve()))
        import_audio = dll.AveMediaBridge_ImportAudioToSession
        import_audio.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
        import_audio.restype = ctypes.c_int

        def encode(options):
            subprocess.run([ffmpeg, '-hide_banner', '-nostdin', '-v', 'error', '-y',
                            *map(str, options)], check=True)

        def read(media):
            output = root / (media.name + '.session')
            rc = import_audio(str(media), str(output))
            assert rc == 0, (media.name, rc)
            meta = json.loads((output / 'metadata.json').read_bytes())
            return meta, (output / 'original_f32.bin').read_bytes()

        def render(frames, rate, channels, scale, suffix, extra=()):
            wave_file = root / 'input.wav'
            signal = array('h', ((i * 2617 + 113) % 26489 - 13244 for i in range(frames * channels)))
            with wave.open(str(wave_file), 'wb') as w:
                w.setparams((channels, 2, rate, frames, 'NONE', 'not compressed'))
                w.writeframes(signal.tobytes())
            path = root / (f'{frames}-{rate}-{channels}-{scale}' + suffix)
            encode(['-i', wave_file, '-c:a', 'aac', '-b:a', '96k', '-movie_timescale', str(scale),
                    *extra, path])
            return path

        # Neither the file name nor its movie metadata participates in admission.
        for frames, rate, channels in [(1023, 48000, 1), (1024, 48000, 1), (1025, 48000, 2),
                                       (8193, 44100, 2), (48113, 48000, 2)]:
            path = render(frames, rate, channels, rate, '.m4a')
            meta, pcm = read(path)
            budget = meta['presentationBudget']
            assert budget['source'] == INDEX_SOURCE and not budget['packetScanExecuted'], (path, budget)
            assert budget['frames'] == frames and len(pcm) == frames * channels * 4, (path, budget)
            # Relocate the index to the front and change unrelated container metadata.
            remux = path.with_suffix('.mp4')
            encode(['-i', path, '-c', 'copy', '-movflags', '+faststart', '-movie_timescale',
                    str(rate), '-metadata', 'comment=independent remux', remux])
            other_meta, other_pcm = read(remux)
            assert other_meta['presentationBudget']['source'] == INDEX_SOURCE, remux
            assert other_pcm == pcm, remux

        # The same AAC access units can be presented with a rounded movie edit.
        # The reference importer honors 21 ms = 1008 samples, not the full 1025.
        rounded, pcm = read(render(1025, 48000, 1, 1000, '.m4a'))
        assert rounded['presentationBudget']['source'] != INDEX_SOURCE, rounded
        assert len(pcm) == 1008 * 4 and rounded['presentationBudget']['packetScanExecuted'], rounded
        fragments, pcm = read(render(12001, 48000, 1, 48000, '.mp4',
                                     ['-movflags', '+frag_keyframe+empty_moov']))
        assert fragments['presentationBudget']['source'] != INDEX_SOURCE and pcm, fragments

        # The presentation cap can hide extra decoded samples from a size-only check.
        clean = render(96001, 48000, 1, 48000, '.m4a')
        unusual = root / 'two-access-units.m4a'
        unusual.write_bytes(duplicate_access_unit(clean))

        def decoded_frames(path):
            raw = subprocess.check_output([ffmpeg, '-hide_banner', '-nostdin', '-v', 'error',
                                           '-i', str(path), '-af', 'asetpts=N/SR/TB',
                                           '-f', 'f32le', 'pipe:1'])
            assert len(raw) % 4 == 0
            return len(raw) // 4

        expected = decoded_frames(clean)
        actual = decoded_frames(unusual)
        assert actual == expected + 1024, (expected, actual)
        session = root / 'physical-count.session'
        assert import_audio(str(unusual), str(session)) != 0, 'extra AAC audio was silently trimmed'
        error = dll.AveMediaBridge_GetLastErrorText
        error.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
        message = ctypes.create_unicode_buffer(2048)
        error(message, len(message))
        assert 'before end trimming' in message.value, message.value
        assert str(expected) in message.value and str(actual) in message.value, message.value
        assert not list(session.glob('*')), 'failed import left a partial session'
        assert import_audio(str(clean), str(session)) == 0, 'healthy retry failed'

        def expect_missing_audio(media, data):
            damaged = root / 'truncated.mp4'
            damaged.write_bytes(data)
            out = root / 'truncated.session'
            rc = import_audio(str(damaged), str(out))
            assert rc != 0, (media, 'published incomplete AAC')
            error(message, len(message))
            assert 'incomplete' in message.value.lower(), (media, message.value)
            assert not list(out.glob('*')), 'partial session survived missing audio'

        for movie_scale in (48000, 1000):
            front = root / f'front-{movie_scale}.mp4'
            encode(['-i', clean, '-c', 'copy', '-movflags', '+faststart',
                    '-movie_timescale', movie_scale, front])
            data = front.read_bytes()
            packets = audio_sample_ranges(data)
            mdat, mdat_end = child(data, 0, len(data), b'mdat')
            assert mdat_end == len(data) and packets[-1][1] == mdat_end
            for cut in [packets[-1][0], packets[len(packets) // 2][0], packets[-1][1] - 1]:
                for size in [mdat_end - mdat, 0, cut - mdat]:
                    partial = bytearray(data[:cut])
                    struct.pack_into('>I', partial, mdat, size)
                    expect_missing_audio((movie_scale, cut, size), partial)

        # These structures intentionally use the ordinary packet scan. It must
        # not certify its own short prefix when the source's index says more exists.
        for name, args in [('gapless-marker', ['-metadata', 'comment=iTunSMPB']),
                           ('start-offset', ['-output_ts_offset', '1']),
                           ('two-tracks', ['-map', '0:a', '-map', '0:a'])]:
            front = root / f'{name}.mp4'
            encode(['-i', clean, *args, '-c', 'copy', '-movflags', '+faststart',
                    '-movie_timescale', 48000, front])
            metadata, _ = read(front)
            assert metadata['presentationBudget']['source'] != INDEX_SOURCE, (name, metadata)
            data = front.read_bytes()
            packets = audio_sample_ranges(data)
            expect_missing_audio(name, data[:packets[-1][0]])

        # A big, valid comment exceeds fast admission's metadata limit. It
        # cannot erase the independent evidence of a missing indexed packet.
        comment = root / 'large-comment.txt'
        comment.write_bytes(b';FFMETADATA1\ncomment=' + b'x' * (4 * 1024 * 1024) + b'\n')
        big = root / 'large-metadata.mp4'
        encode(['-i', clean, '-f', 'ffmetadata', '-i', comment, '-map_metadata', '1',
                '-c', 'copy', '-movflags', '+faststart', '-movie_timescale', 48000, big])
        data = big.read_bytes()
        moov, moov_end = child(data, 0, len(data), b'moov')
        assert moov_end - moov > 4 * 1024 * 1024
        metadata, pcm = read(big)
        assert metadata['presentationBudget']['source'] != INDEX_SOURCE and len(pcm) == 96001 * 4
        expect_missing_audio('large metadata', data[:audio_sample_ranges(data)[-1][0]])

        main_profile = render(96005, 48000, 1, 48000, '.mp4',
                              ['-profile:a', 'aac_main', '-movflags', '+faststart'])
        metadata, pcm = read(main_profile)
        assert metadata['presentationBudget']['source'] != INDEX_SOURCE and len(pcm) == 96005 * 4
        data = main_profile.read_bytes()
        expect_missing_audio('AAC Main', data[:audio_sample_ranges(data)[-1][0]])

        # Physical EOF during video is compatible with fully available selected audio.
        video = root / 'longer-video.mp4'
        encode(['-f', 'lavfi', '-i', 'color=s=32x32:r=5:d=4', '-i', clean, '-map', '0:v',
                '-map', '1:a', '-c:v', 'mpeg4', '-c:a', 'copy', '-movie_timescale', 48000,
                '-movflags', '+faststart', video])
        media = video.read_bytes()
        audio_end = max(end for _, end in audio_sample_ranges(media))
        assert audio_end < len(media) - 16
        video_cut = root / 'incomplete-video-complete-audio.mp4'
        video_cut.write_bytes(media[:audio_end])
        _, reference_pcm = read(video)
        _, truncated_video_pcm = read(video_cut)
        assert truncated_video_pcm == reference_pcm

        # The selected (first, default) track is short. Only the second track
        # is missing packets after truncation.
        short = render(8193, 48000, 1, 48000, '.m4a')
        tracks = root / 'short-default-long-alternate.mp4'
        encode(['-i', short, '-i', clean, '-map', '0:a', '-map', '1:a', '-c', 'copy',
                '-disposition:a:0', 'default', '-disposition:a:1', '0',
                '-movie_timescale', 48000, '-movflags', '+faststart', tracks])
        media = tracks.read_bytes()
        audio_end = max(end for _, end in audio_sample_ranges(media))
        assert audio_end < len(media) - 16
        cut_tracks = root / 'selected-complete-alternate-missing.mp4'
        cut_tracks.write_bytes(media[:audio_end])
        _, chosen = read(tracks)
        _, chosen_cut = read(cut_tracks)
        assert len(chosen) == 8193 * 4 and chosen_cut == chosen
    print('AAC_RUNTIME_INDEX_IMPORT_OK')


if __name__ == '__main__':
    exercise(*sys.argv[1:])

"""Compare pinned-file inspection to an independent decode and exercise clean fallback."""
import ctypes as C
import hashlib
import json
import msvcrt
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zlib


def elements(data, start, end):
    def vint(pos, keep_marker):
        width = 9 - data[pos].bit_length()
        assert 1 <= width <= 8 and pos + width <= end
        value = int.from_bytes(data[pos:pos+width], 'big')
        return (value if keep_marker else value ^ (1 << (width*7))), pos+width
    while start < end:
        identifier, pos = vint(start, True)
        size, content = vint(pos, False)
        assert content + size <= end
        yield identifier, start, content, content + size
        start = content + size


def omit_audio_packet(source, target):
    data = bytearray(source.read_bytes())
    segment = next(e for e in elements(data, 0, len(data)) if e[0] == 0x18538067)
    clusters = [e for e in elements(data, *segment[2:]) if e[0] == 0x1f43b675]
    for cluster in clusters:
        children = list(elements(data, *cluster[2:]))
        blocks = [e for e in children if e[0] == 0xa3 and data[e[2]] == 0x82]
        if len(blocks) < 3:
            continue
        removed = blocks[len(blocks)//2]
        data[removed[1]] = 0xec  # Same-size Void leaves all later file offsets intact.
        for ident, at, pos, end in children:
            if ident == 0xbf:
                assert end - pos == 4
                crc = zlib.crc32(data[cluster[2]:at] + data[end:cluster[3]])
                data[pos:end] = crc.to_bytes(4, 'little')
        target.write_bytes(data)
        return
    raise AssertionError('No interior AAC block in fixture')


def main(dll_path, ffmpeg_dir, ffmpeg):
    cancel_type = C.CFUNCTYPE(C.c_int, C.c_void_p)
    with os.add_dll_directory(ffmpeg_dir):
        dll = C.CDLL(dll_path)
    inspect = dll.AveMediaBridge_InspectMatroskaAacSourceV1
    u8, u64, i32 = C.c_uint8, C.c_uint64, C.c_int32
    inspect.argtypes = [C.c_wchar_p, C.c_void_p, cancel_type, C.c_void_p, C.POINTER(u8),
                        C.POINTER(u64), C.POINTER(u64), C.POINTER(i32), C.POINTER(i32), C.POINTER(i32)]
    inspect.restype = C.c_int
    create = C.WinDLL('kernel32', use_last_error=True).CreateFileW
    create.argtypes = [C.c_wchar_p, C.c_uint32, C.c_uint32, C.c_void_p, C.c_uint32, C.c_uint32, C.c_void_p]
    create.restype = C.c_void_p

    def check(path, status, frames=None, cancel=False, label=None):
        handle = create(str(path), 0x80000000, 1, None, 3, 0, None)
        assert handle != C.c_void_p(-1).value, C.get_last_error()
        with os.fdopen(msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY),'rb',buffering=0) as pin:
            pin.seek(7)
            sentinel = 0xdb
            sha = (u8*32)(*([sentinel]*32))
            values = [cls(sentinel) for cls in (u64,u64,i32,i32,i32)]
            actual = inspect(str(label or path), handle, cancel_type(lambda _: cancel), None, sha,
                             *(C.byref(v) for v in values))
            assert pin.tell() == 7
        assert actual == status, (path, actual, status)
        if status == 1:
            assert bytes(sha) == hashlib.sha256(path.read_bytes()).digest()
            assert [v.value for v in values] == [path.stat().st_size, frames, 1, 48000, 2]
        else:
            assert bytes(sha) == bytes([sentinel])*32 and all(v.value == sentinel for v in values)

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        aac, media, gap = root/'tone.aac', root/'video.mkv', root/'gap.mkv'
        def ff(*args):
            return subprocess.check_output([ffmpeg, '-nostdin', '-v', 'error', *args])
        # ADTS carries no Matroska CodecDelay, so the muxed stream exercises the strict proof.
        ff('-f','lavfi','-i','sine=frequency=300:sample_rate=48000:duration=2',
           '-c:a','aac','-ac','2','-f','adts',str(aac))
        ff('-f','lavfi','-i','testsrc2=size=96x64:rate=15:duration=2.5','-i',str(aac),
           '-map','0:v:0','-map','1:a:0','-c:v','mjpeg','-c:a','copy',str(media))
        def decode(path):
            pcm = ff('-i',str(path),'-map','0:a:0','-c:a','pcm_f32le','-f','f32le','pipe:1')
            assert len(pcm) % 8 == 0
            return len(pcm)//8
        frames = decode(media)
        assert frames > 2048
        label = root/'display-label-only.mkv'
        label.write_bytes(b'The borrowed handle supplies all media bytes.')
        check(media, 1, frames, label=label)
        omit_audio_packet(media, gap)
        assert decode(gap) == frames - 1024
        check(gap, 0)
        check(media, -1, cancel=True)

        # Keep the exact same AAC packets and move the later pictures far enough
        # away that treating the whole movie as PCM would exceed the free disk.
        long_video = root / 'long-picture.mkv'
        free = shutil.disk_usage(root).free
        delay_ms = 2 * free // (48000 * 2 * 4 // 1000) + 1000
        shifted = f"setts=pts='PTS+gt(N,0)*{delay_ms}':dts='DTS+gt(N,0)*{delay_ms}'"
        ff('-i', str(media), '-map', '0', '-c', 'copy', '-bsf:v',
           shifted, str(long_video))
        probe = dll.AveMediaBridge_ProbeToJson
        probe.argtypes = [C.c_wchar_p, C.c_wchar_p]
        document = root / 'long-picture.json'
        assert probe(str(long_video), str(document)) == 0
        details = json.loads(document.read_text(encoding='utf-8-sig'))
        assert details['decodedSampleFramesSource'] == 'format_duration_estimate', details
        assert details['estimatedDecodedBytes'] > free, (details['estimatedDecodedBytes'], free)

        importer = dll.AveMediaBridge_ImportAudioToSession
        importer.argtypes = [C.c_wchar_p, C.c_wchar_p]
        get_error = dll.AveMediaBridge_GetLastErrorText
        get_error.argtypes = [C.POINTER(C.c_wchar), C.c_int]
        reason = C.create_unicode_buffer(1024)
        native_pcm = []
        for source in (media, long_video):
            output = root / source.stem
            if importer(str(source), str(output)) != 0:
                get_error(reason, len(reason))
                raise AssertionError(f'{source.name}: {reason.value}')
            native_pcm.append((output / 'original_f32.bin').read_bytes())
        assert len(native_pcm[0]) == frames * 8 and native_pcm[0] == native_pcm[1]

        # With no picture track, the same large duration is still subject to
        # the existing disk preflight. No large file is written by this test.
        audio_only = root / 'audio-only.mka'
        ff('-i', str(media), '-map', '0:a:0', '-c', 'copy', '-bsf:a', shifted, str(audio_only))
        refused = root / 'refused'
        assert importer(str(audio_only), str(refused)) != 0
        get_error(reason, len(reason))
        assert 'not enough free disk space' in reason.value, reason.value
        assert not (refused / 'original_f32.bin').exists()


if __name__ == '__main__':
    main(*sys.argv[1:])

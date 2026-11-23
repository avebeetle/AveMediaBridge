"""Exercise the optional Ogg reader on an immutable source after a bounded probe refusal."""
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

U32 = c.c_uint32
READ = c.CFUNCTYPE(U32, c.c_void_p, c.c_uint64, c.c_void_p, U32, c.POINTER(U32))
CHECK = c.CFUNCTYPE(U32, c.c_void_p)


class Source(c.Structure):
    _fields_ = [('size', U32), ('version', U32), ('bytes', c.c_uint64),
                ('token', c.c_uint8 * 16), ('user', c.c_void_p), ('read', READ),
                ('check', CHECK), ('reserved', U32 * 4)]


class Prepare(c.Structure):
    _fields_ = [('size', U32), ('version', U32), ('source', c.POINTER(Source)),
                ('label', c.c_wchar_p), ('reserved', U32 * 4)]


class Import(c.Structure):
    _fields_ = [('size', U32), ('version', U32), ('directory', c.c_wchar_p),
                ('progress', c.c_void_p), ('cancel', c.c_void_p), ('user', c.c_void_p),
                ('wave', c.c_void_p), ('reserved', U32 * 4)]


def main(library, ffmpeg_bin, ffmpeg):
    def encode(*args):
        return subprocess.check_output([ffmpeg, '-nostdin', '-v', 'error', *args])

    encoders = encode('-encoders').decode(errors='replace')
    if 'libvorbis' not in encoders:
        return 77

    with os.add_dll_directory(ffmpeg_bin), tempfile.TemporaryDirectory() as directory:
        dll = c.CDLL(library)
        dll.AveMediaBridge_ProbeToJson.argtypes = [c.c_wchar_p, c.c_wchar_p]
        dll.AveMediaBridge_ImportAudioToSession.argtypes = [c.c_wchar_p, c.c_wchar_p]
        prepare = dll.AveMediaBridge_ReaderPrepareVorbisV1
        prepare.argtypes = [c.POINTER(Prepare), c.POINTER(c.c_void_p)]
        dll.AveMediaBridge_ReaderWriteProbeJsonV1.argtypes = [c.c_void_p, c.c_wchar_p]
        dll.AveMediaBridge_ReaderImportGuardedV1.argtypes = [c.c_void_p, c.POINTER(Import)]
        dll.AveMediaBridge_ReaderDestroyV1.argtypes = [c.c_void_p]
        dll.AveMediaBridge_GetLastErrorText.argtypes = [c.POINTER(c.c_wchar), c.c_int]

        root = Path(directory)
        base, tagged = root/'base.ogg', root/'tagged.ogg'
        encode('-f', 'lavfi', '-i', 'sine=frequency=723:sample_rate=48000:duration=1',
               '-c:a', 'libvorbis', '-ac', '2', str(base))
        metadata = root/'comment.txt'
        metadata.write_text(';FFMETADATA1\ncomment=' + 'v'*(5*1024*1024), encoding='ascii')
        encode('-i', str(base), '-i', str(metadata), '-map', '0:a', '-map_metadata', '1',
               '-c:a', 'copy', str(tagged))
        session = root/'ordinary'
        assert dll.AveMediaBridge_ImportAudioToSession(str(base), str(session)) == 0
        reference = (session/'original_f32.bin').read_bytes()
        assert len(reference) == 48000 * 8
        proof = root/'quick.json'
        assert dll.AveMediaBridge_ProbeToJson(str(tagged), str(proof)) != 0
        assert json.loads(proof.read_text(encoding='utf-8-sig'))['mediaOpenReason'] == 'quick_scan_limit'

        original = tagged.read_bytes()
        unsupported = {
            '5.1': 'AveVoice cannot import the 5.1 channel layout in this Vorbis file.',
            'quad': 'AveVoice cannot import the multichannel layout in this Vorbis file.',
        }
        for case in ('valid', 'truncated', 'crc', 'cancel-prepare', 'cancel-import', 'io', *unsupported):
            data = original
            if case == 'truncated':
                data = original[:-2]
            elif case == 'crc':
                data = original[:-1] + bytes([original[-1] ^ 1])
            elif case in unsupported:
                last_link = root/(case+'.ogg')
                encode('-f', 'lavfi', '-i', 'anullsrc=r=48000:cl='+case, '-t', '0.1',
                       '-c:a', 'libvorbis', str(last_link))
                data = original + last_link.read_bytes()
            state = {'scan_blocks': 0, 'cancel': False}
            def read(_, offset, out, n, got):
                got[0] = 0
                if n == 65536: state['scan_blocks'] += 1
                if case == 'io' and state['scan_blocks'] >= 3: return 4
                part = data[offset:offset+n]
                c.memmove(out, part, len(part))
                got[0] = len(part)
                return 0

            def check(_):
                return 2 if (state['cancel'] or
                             case == 'cancel-prepare' and state['scan_blocks'] >= 3) else 0

            descriptor = Source(c.sizeof(Source), 1, len(data), (c.c_uint8*16)(1), None,
                                READ(read), CHECK(check), (U32*4)())
            # Every read must use the pin, even when the label has no file at all.
            options = Prepare(c.sizeof(Prepare), 1, c.pointer(descriptor), str(root/'display-only.ogg'), (U32*4)())
            handle = c.c_void_p()
            status = prepare(c.byref(options), c.byref(handle))
            if case in unsupported:
                assert status == 2 and not handle.value, (case, status)
                message = c.create_unicode_buffer(512)
                assert dll.AveMediaBridge_GetLastErrorText(message, len(message)) == 0
                assert message.value == unsupported[case], (case, message.value)
                continue
            if case in ('cancel-prepare', 'io', 'truncated', 'crc'):
                assert status != 0 and not handle.value, (case, status)
                if case == 'cancel-prepare': assert status == 4, status
                continue
            assert status == 0 and handle.value, (case, status)
            try:
                j = root/(case+'.json')
                assert dll.AveMediaBridge_ReaderWriteProbeJsonV1(handle, str(j)) == 0
                assert json.loads(j.read_text(encoding='utf-8-sig'))['decodedSampleFrames'] == 48000
                dest = root/case
                dest.mkdir()
                entry = Import(c.sizeof(Import), 1, str(dest))
                state['cancel'] = case == 'cancel-import'
                code = dll.AveMediaBridge_ReaderImportGuardedV1(handle, c.byref(entry))
                if case == 'cancel-import':
                    assert code == 4 and not list(dest.iterdir()), code
                else:
                    assert code == 0
                    assert (dest/'original_f32.bin').read_bytes() == reference
                    assert dll.AveMediaBridge_ReaderImportGuardedV1(handle, c.byref(entry)) == 7
            finally:
                dll.AveMediaBridge_ReaderDestroyV1(handle)
        print('Extended Vorbis preparation, cancellation and pinned PCM verified',
              hashlib.sha256(reference).hexdigest())
    return 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))

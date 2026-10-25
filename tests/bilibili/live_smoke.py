"""Bounded real-network check. Stores no account cookies or signed CDN addresses."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--runtime', help='Existing Veyra runtime directory; read only')
    parser.add_argument('--output', required=True)
    parser.add_argument('--video', default='BV1xx411c7mD')
    parser.add_argument('--live', action='store_true', help='Check live audio/video without seeking')
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    report = {'video': args.video, 'runtime': args.runtime, 'checks': {}}
    process = subprocess.Popen([args.probe, args.video], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding='utf-8', errors='replace', creationflags=subprocess.CREATE_NO_WINDOW)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line.rstrip()) for line in process.stdout], daemon=True).start()
    started = time.monotonic()
    try:
        url = None
        while time.monotonic() - started < 50:
            try:
                line = lines.get(timeout=1)
            except queue.Empty:
                if process.poll() is not None:
                    raise RuntimeError(f'Parser exited: {process.returncode}')
                continue
            if line.startswith('READY '):
                url = line.split(' ', 2)[1]
                break
            print(line, flush=True)
        if not url or not url.startswith('http://127.0.0.1:'):
            raise RuntimeError('No loopback media URL within 50 seconds')
        report['checks']['resolve_seconds'] = round(time.monotonic() - started, 3)
        ffprobe = str(Path(args.ffmpeg).with_name('ffprobe.exe'))
        probe = subprocess.run([ffprobe, '-v', 'error', '-rw_timeout', '15000000', '-show_entries',
                                'stream=codec_type,codec_name,width,height:format=duration', '-of', 'json', url],
                               capture_output=True, timeout=45, creationflags=subprocess.CREATE_NO_WINDOW)
        if probe.returncode:
            raise RuntimeError('ffprobe: ' + probe.stderr.decode('utf-8', 'replace'))
        report['checks']['media'] = json.loads(probe.stdout)
        streams = report['checks']['media']['streams']
        assert {'video', 'audio'} <= {stream['codec_type'] for stream in streams}
        decode_key = 'decode_live' if args.live else 'decode_after_seek'
        decode = subprocess.run([args.ffmpeg, '-nostdin', '-hide_banner', '-v', 'error', '-rw_timeout', '15000000'] +
                                ([] if args.live else ['-ss', '30']) +
                                ['-i', url, '-t', '5', '-map', '0:v:0', '-map', '0:a:0', '-f', 'null', '-'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=60,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        report['checks'][decode_key] = {'exit': decode.returncode,
            'stderr': re.sub(r'https?://\S+', '[media-url]', decode.stderr.decode('utf-8', 'replace'))}
        if decode.returncode:
            raise RuntimeError('Actual audio/video decoding failed: ' + report['checks'][decode_key]['stderr'])
        if args.runtime:
            with os.add_dll_directory(args.runtime):
                fmt = ctypes.CDLL(str(Path(args.runtime) / 'avformat-63.dll'))
                codec = ctypes.CDLL(str(Path(args.runtime) / 'avcodec-63.dll'))
                util = ctypes.CDLL(str(Path(args.runtime) / 'avutil-61.dll'))
                util.av_log_set_level(-8)
                fmt.avformat_open_input.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p]
                fmt.avformat_find_stream_info.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
                fmt.av_find_best_stream.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
                fmt.av_read_frame.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
                fmt.av_seek_frame.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int64, ctypes.c_int]
                fmt.avformat_close_input.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
                codec.av_packet_alloc.restype = ctypes.c_void_p
                codec.av_packet_unref.argtypes = [ctypes.c_void_p]
                codec.av_packet_free.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
                context = ctypes.c_void_p()
                assert fmt.avformat_open_input(ctypes.byref(context), url.encode(), None, None) == 0
                try:
                    assert fmt.avformat_find_stream_info(context, None) >= 0
                    assert fmt.av_find_best_stream(context, 0, -1, -1, None, 0) >= 0
                    assert fmt.av_find_best_stream(context, 1, -1, -1, None, 0) >= 0
                    packet = ctypes.c_void_p(codec.av_packet_alloc())
                    try:
                        for seek_to in ((0, 0) if args.live else (0, 30_000_000)):
                            if seek_to:
                                assert fmt.av_seek_frame(context, -1, seek_to, 1) >= 0
                            for _ in range(64):
                                assert fmt.av_read_frame(context, packet) >= 0
                                codec.av_packet_unref(packet)
                    finally:
                        codec.av_packet_free(ctypes.byref(packet))
                finally:
                    fmt.avformat_close_input(ctypes.byref(context))
                report['checks']['veyra_runtime'] = 'audio/video streams, 128 packets passed' + ('' if args.live else ', seek to 30 s passed')
        report['passed'] = True
    except Exception as error:
        report['passed'] = False
        report['error'] = re.sub(r'https?://\S+', '[media-url]', str(error))
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'live-smoke.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

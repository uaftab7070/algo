#!/usr/bin/env python3
"""Loopback-only preview server. Chrome owns camera access; OpenCV runs locally."""
import argparse
import hashlib
import json
import re
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

import cv2
import numpy as np
from precision_tracker import PrecisionTracker, MODEL_DIR

ROOT = Path(__file__).resolve().parent
SAMPLES = {'football': Path('/Users/apple/Downloads/Tracking Videos/Football.mp4'),
           'birds': Path('/Users/apple/Downloads/video.mp4')}
SESSIONS = {}
SESSION_LOCK = threading.Lock()
MAX_SESSIONS = 4


def models_ready():
    try:
        manifest = json.loads((MODEL_DIR / 'provenance.json').read_text())
        return all(hashlib.sha256((MODEL_DIR / item['name']).read_bytes()).hexdigest() == item['sha256']
                   for item in manifest['files'])
    except (OSError, ValueError, KeyError):
        return False


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def log_message(self, fmt, *args):
        # Frame requests are frequent; keep errors visible without per-frame log spam.
        if args and str(args[0]).startswith('POST /api/track'):
            return
        super().log_message(fmt, *args)

    def json_response(self, value, status=200):
        data = json.dumps(value, allow_nan=False, separators=(',', ':')).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == '/api/health':
            available = models_ready()
            return self.json_response({'ready': available, 'engine': 'NanoTrack + LK',
                                       'samples': [name for name, p in SAMPLES.items() if p.is_file()]})
        if path.startswith('/api/sample/'):
            name = path.rsplit('/', 1)[-1]
            sample = SAMPLES.get(name)
            if sample is None or not sample.is_file():
                return self.json_response({'error': 'Sample unavailable'}, 404)
            length = sample.stat().st_size
            start, end = 0, length - 1
            requested = self.headers.get('Range')
            if requested:
                match = re.fullmatch(r'bytes=(\d+)-(\d*)', requested)
                if not match:
                    return self.json_response({'error': 'Unsupported byte range'}, 416)
                start = int(match[1])
                end = min(length - 1, int(match[2])) if match[2] else length - 1
                if start > end:
                    return self.json_response({'error': 'Invalid range'}, 416)
            self.send_response(206 if requested else 200)
            self.send_header('Content-Type', 'video/mp4')
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Content-Length', str(end - start + 1))
            if requested:
                self.send_header('Content-Range', f'bytes {start}-{end}/{length}')
            self.end_headers()
            try:
                with sample.open('rb') as source:
                    source.seek(start)
                    remaining = end - start + 1
                    while remaining > 0:
                        chunk = source.read(min(1024 * 1024, remaining))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        remaining -= len(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if path.startswith('/.') or '/.' in path:
            return self.send_error(404)
        super().do_GET()

    def do_POST(self):
        if urlsplit(self.path).path != '/api/track':
            return self.json_response({'error': 'Unknown endpoint'}, 404)
        origin = self.headers.get('Origin')
        port = self.server.server_port
        if origin and origin not in (f'http://127.0.0.1:{port}', f'http://localhost:{port}'):
            return self.json_response({'error': 'This endpoint is for the local preview only'}, 403)
        try:
            session_id = self.headers.get('X-Tracker-Session', '')
            if not re.fullmatch(r'[a-zA-Z0-9-]{8,80}', session_id):
                return self.json_response({'error': 'Invalid preview session'}, 400)
            action = self.headers.get('X-Tracker-Action', 'update')
            with SESSION_LOCK:
                now = time.monotonic()
                for key in list(SESSIONS):
                    if now - SESSIONS[key]['used'] > 600:
                        del SESSIONS[key]
                if action == 'reset':
                    SESSIONS.pop(session_id, None)
                    return self.json_response({'ok': True})
                if session_id not in SESSIONS:
                    if action != 'init':
                        return self.json_response({'error': 'Select the target to initialize the tracker'}, 409)
                    if len(SESSIONS) >= MAX_SESSIONS:
                        return self.json_response({'error': 'Close another tracking tab and try again'}, 429)
                    SESSIONS[session_id] = {'tracker': PrecisionTracker(), 'lock': threading.Lock(), 'used': now}
                session = SESSIONS[session_id]
                session['used'] = now
            length = int(self.headers.get('Content-Length', '0'))
            if length <= 0 or length > 5 * 1024 * 1024:
                return self.json_response({'error': 'Frame size is invalid'}, 413)
            data = self.rfile.read(length)
            frame = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
            if frame is None or frame.shape[0] * frame.shape[1] > 5_000_000:
                return self.json_response({'error': 'Invalid image frame'}, 400)
            timestamp = float(self.headers.get('X-Timestamp-Ms', '0'))
            with session['lock']:
                if action == 'init':
                    result = session['tracker'].initialize(frame, json.loads(self.headers['X-Tracker-Roi']), timestamp)
                else:
                    result = session['tracker'].update(frame, timestamp)
            result['frame_id'] = self.headers.get('X-Frame-Id', '')
            result['timestamp_ms'] = timestamp
            self.json_response(result)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            self.json_response({'error': str(error)}, 400)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8765)
    args = parser.parse_args()
    if not models_ready():
        raise SystemExit('NanoTrack models are missing or invalid. Run: python3 tools/download_models.py')
    cv2.setNumThreads(2)
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    print(f'Neural preview ready: http://127.0.0.1:{args.port}/camera_preview.html', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.server_close()

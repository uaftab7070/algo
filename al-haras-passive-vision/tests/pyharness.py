"""Shared harness for integration, fault and acceptance tests (stdlib only).

Environment:
  AHPV_BIN      path to the ahpv binary (default build/host/ahpv)
  AHPV_TEST_TMP directory for temporary data (default: system temp)
"""
import http.client
import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get('AHPV_BIN', os.path.join(ROOT, 'build', 'host', 'ahpv'))
TMP = os.environ.get('AHPV_TEST_TMP') or tempfile.gettempdir()
VERIFIER = os.path.join(ROOT, 'tools', 'verify_artifacts.py')
NOTICE = 'NOT FOR FLIGHT OR ENGAGEMENT CONTROL'
_fixture_lock = threading.Lock()


def fixtures():
    """Generates the synthetic corpus once per test process."""
    path = os.path.join(TMP, 'ahpv-fixtures')
    with _fixture_lock:
        if not os.path.exists(os.path.join(path, 'corpus-manifest.json')):
            subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tests', 'fixtures', 'make_fixtures.py'), path], check=True)
    return path


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Result:
    def __init__(self, proc):
        self.code = proc.returncode
        self.stdout = proc.stdout
        self.stderr = proc.stderr
        self.json = None
        for line in reversed(proc.stdout.strip().splitlines()):
            try:
                self.json = json.loads(line)
                break
            except ValueError:
                continue


class Env:
    ROLES = {'op': 'operator', 'ann': 'annotator', 'ann2': 'annotator', 'rev': 'reviewer', 'view': 'viewer'}

    def __init__(self, label, source='bench_motion.y4m', kind='y4m_file', extra=None):
        self.root = tempfile.mkdtemp(prefix=f'ahpv-{label}-', dir=TMP)
        self.data = os.path.join(self.root, 'data')
        self.config = os.path.join(self.root, 'config.yaml')
        self.port = free_port()
        # Stream endpoints (FIFOs) are private to each test; files come from the shared corpus.
        corpus = fixtures()  # Always generated up front: stream feeders read from it.
        base = self.root if kind == 'y4m_stream' else corpus
        self.source = source if os.path.isabs(source) else os.path.join(base, source)
        self.write_config(kind, extra or {})
        self.tokens = {}
        for name, role in self.ROLES.items():
            result = self.cli('auth', 'add', '--principal', name, '--role', role, token=False)
            assert result.code == 0, result.stderr
            path = os.path.join(self.root, f'{name}.token')
            with open(path, 'w') as f:
                f.write(result.json['token'] + '\n')
            os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
            self.tokens[name] = path

    def write_config(self, kind, extra):
        values = {
            'data_root': self.data, 'source.kind': kind, 'source.path': self.source,
            'source.no_signal_timeout_ms': '500', 'recording.fsync_interval_frames': '5',
            'storage.warn_free_bytes': '1048576', 'storage.reserve_free_bytes': '524288',
            'metrics.sample_interval_ms': '200', 'preview.port': str(self.port), 'logging.level': 'warn',
            'logging.file': os.path.join(self.root, 'ahpv.log'),
        }
        values.update(extra)
        sections = {}
        for key, value in values.items():
            parts = key.split('.')
            node = sections
            for part in parts[:-1]:
                node = node.setdefault(part, {})
            node[parts[-1]] = value

        def emit(node, indent=0):
            out = ''
            for key, value in node.items():
                if isinstance(value, dict):
                    out += ' ' * indent + key + ':\n' + emit(value, indent + 2)
                else:
                    out += ' ' * indent + f'{key}: "{value}"\n'
            return out

        with open(self.config, 'w') as f:
            f.write(emit(sections))

    def cli(self, *args, token='op', timeout=180, config=True):
        argv = [BIN, *args]
        if config:
            argv += ['--config', self.config]
        if token:
            argv += ['--token-file', self.tokens[token]]
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return Result(proc)

    def record(self, *extra, token='op'):
        return self.cli('run', '--purpose', 'automated test', '--partition', 'development', '--exit-on-end', *extra, token=token)

    def start(self, *extra, token='op'):
        argv = [BIN, 'run', '--config', self.config, '--token-file', self.tokens[token], '--purpose', 'automated test',
                '--partition', 'development', *extra]
        return subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def sessions(self):
        return [s['session_id'] for s in self.cli('sessions', token=False).json['sessions']]

    def session_dir(self, session_id):
        return os.path.join(self.data, 'sessions', session_id)

    def manifest(self, session_id):
        with open(os.path.join(self.session_dir(session_id), 'session.json')) as f:
            return json.load(f)

    def lines(self, session_id, name):
        path = os.path.join(self.session_dir(session_id), name)
        with open(path) as f:
            return [json.loads(l) for l in f if l.endswith('\n')]

    def token(self, name):
        with open(self.tokens[name]) as f:
            return f.read().strip()

    def http(self, method, path, token=None, body=None, host=None, origin=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        headers = {'Host': host or f'127.0.0.1:{self.port}'}
        if token:
            headers['Authorization'] = 'Bearer ' + self.token(token)
        if origin:
            headers['Origin'] = origin
        payload = None
        if body is not None:
            payload = body if isinstance(body, bytes) else json.dumps(body).encode()
            headers['Content-Type'] = 'application/json'
        conn.request(method, path, body=payload, headers=headers)
        response = conn.getresponse()
        data = response.read()
        conn.close()
        return response.status, dict(response.getheaders()), data

    def wait_http(self, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                return self.http('GET', '/api/health')
            except OSError:
                time.sleep(0.05)
        raise TimeoutError('preview did not start')

    def audit(self):
        with open(os.path.join(self.data, 'audit.jsonl')) as f:
            return [json.loads(l) for l in f]


def verify_independently(kind, path):
    proc = subprocess.run([sys.executable, '-I', VERIFIER, kind, path], capture_output=True, text=True)
    return proc.returncode, json.loads(proc.stdout)

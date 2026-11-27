"""Unit and isolation tests for the shared Gemma 4 E4B engine and dual gateways."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import shared_gemma as sg


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class FakeSharedLlamaHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, data, status=200):
        raw = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def check_engine_auth(self):
        auth = self.headers.get('Authorization', '')
        if auth != f'Bearer {self.server.engine_api_key}':
            self.send_json({'error': {'message': 'Invalid API Key', 'code': 401}}, 401)
            return False
        return True

    def do_GET(self):
        if self.path == '/health':
            return self.send_json({'status': 'ok'})
        if not self.check_engine_auth():
            return
        if self.path == '/props':
            return self.send_json({
                'model_alias': sg.ENGINE_ALIAS,
                'model_path': str(self.server.model_path),
                'chat_template': '<|turn>user\n<turn|>',
                'default_generation_settings': {'n_ctx': sg.CONTEXT},
            })
        if self.path == '/v1/models':
            return self.send_json({'data': [{'id': sg.ENGINE_ALIAS}]})
        self.send_json({'detail': 'Not found'}, 404)

    def do_POST(self):
        if not self.check_engine_auth():
            return
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append((self.path, body))
        if self.path == '/apply-template':
            return self.send_json({'prompt': json.dumps(body, ensure_ascii=False)})
        if self.path == '/tokenize':
            return self.send_json({'tokens': [1] * 42})
        if self.path == '/v1/chat/completions':
            if self.server.delay > 0:
                time.sleep(self.server.delay)
            usage = {
                'prompt_tokens': 42,
                'completion_tokens': 4,
                'prompt_tokens_details': {'cached_tokens': self.server.cached_tokens},
            }
            if not body.get('stream'):
                return self.send_json({
                    'id': 'fake-shared',
                    'choices': [{'message': {'role': 'assistant', 'content': 'Merkezi yanıt.'}, 'finish_reason': 'stop'}],
                    'usage': usage,
                })
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            pkt1 = {'choices': [{'delta': {'content': 'Merkezi '}, 'finish_reason': None}]}
            pkt2 = {'choices': [{'delta': {'content': 'yanıt.'}, 'finish_reason': 'stop'}], 'usage': usage}
            self.wfile.write(b'data: ' + json.dumps(pkt1).encode() + b'\n\n')
            self.wfile.flush()
            self.wfile.write(b'data: ' + json.dumps(pkt2).encode() + b'\n\n')
            self.wfile.write(b'data: [DONE]\n\n')
            self.wfile.flush()


class FakeEngineWrapper:
    def __init__(self, config, model_path):
        self.config = config
        self.model_path = model_path
        self.engine_api_key = os.urandom(32).hex()
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), sg.NoRedirect())
        self.httpd = ThreadingHTTPServer(('127.0.0.1', config.engine_port), FakeSharedLlamaHandler)
        self.httpd.daemon_threads = True
        self.httpd.model_path = model_path
        self.httpd.engine_api_key = self.engine_api_key
        self.httpd.requests = []
        self.httpd.delay = 0.0
        self.httpd.cached_tokens = 0
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def api(self, path, body=None, timeout=15):
        req = urllib.request.Request(
            f'http://127.0.0.1:{self.config.engine_port}{path}',
            data=None if body is None else json.dumps(body, ensure_ascii=False).encode(),
            headers={
                'Content-Type': 'application/json',
                'Authorization': f'Bearer {self.engine_api_key}',
            },
        )
        with self.opener.open(req, timeout=timeout) as r:
            return json.load(r)

    def verify_identity(self):
        return 'fake-template-sha256'

    def close(self):
        self.httpd.shutdown()
        self.thread.join(timeout=2)
        self.httpd.server_close()


class SharedGatewayIsolationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='shared-gemma-unit-')
        self.root = Path(self.tmp.name)
        self.fake_file = self.root / 'fake.bin'
        self.fake_file.write_text('fake')
        self.sys_file = self.root / 'gemma-system.md'
        self.sys_file.write_text('ZCI SOHBET SISTEM TALIMATI')
        self.e_port = free_port()
        self.zci_port = free_port()
        self.zek_port = free_port()
        self.cfg = sg.SharedConfig(
            engine=self.fake_file,
            model=self.fake_file,
            model_sha256='0' * 64,
            zci_system_file=self.sys_file,
            agent_base=self.root,
            start_sha256='0' * 64,
            runner_sha256='0' * 64,
            engine_port=self.e_port,
            zci_gateway_port=self.zci_port,
            zekans_gateway_port=self.zek_port,
            busy_wait_seconds=0.0,
        )
        self.engine = FakeEngineWrapper(self.cfg, self.fake_file)
        self.coord = sg.SharedCoordinator(self.engine)
        self.dual = sg.DualGatewayServer(self.coord).start()
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.zci_headers = {'Authorization': 'Bearer local'}
        self.task_token = os.urandom(32).hex()
        self.task_sha = hashlib.sha256(self.task_token.encode()).hexdigest()
        self.coord.bind_zekans_task(os.getpid(), self.task_sha, budget=2)
        self.zek_headers = {'Authorization': f'Bearer {self.task_token}'}

    def tearDown(self):
        self.dual.close()
        self.engine.close()
        self.tmp.cleanup()

    def post(self, port, path, body, headers=None):
        hdrs = {'Content-Type': 'application/json'}
        if headers:
            hdrs.update(headers)
        req = urllib.request.Request(
            f'http://127.0.0.1:{port}{path}',
            data=json.dumps(body, ensure_ascii=False).encode(),
            headers=hdrs,
        )
        try:
            with self.opener.open(req, timeout=10) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            raw = e.read()
            e.close()
            return e.code, json.loads(raw)

    def get(self, port, path, headers=None):
        req = urllib.request.Request(
            f'http://127.0.0.1:{port}{path}',
            headers=headers or {},
        )
        try:
            with self.opener.open(req, timeout=10) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            raw = e.read()
            e.close()
            return e.code, json.loads(raw)

    def test_direct_engine_port_and_gateway_auth_boundaries(self):
        # 1. Direct loopback access to internal engine port without engine key is rejected (401)
        code, _ = self.get(self.e_port, '/v1/models')
        self.assertEqual(code, 401)
        code, _ = self.post(self.e_port, '/v1/chat/completions', {
            'model': sg.ENGINE_ALIAS,
            'messages': [{'role': 'user', 'content': 'Merhaba'}],
        })
        self.assertEqual(code, 401)
        # Engine port also rejects ZCI token and ZEKANS task token
        code, _ = self.get(self.e_port, '/props', headers=self.zci_headers)
        self.assertEqual(code, 401)
        code, _ = self.get(self.e_port, '/props', headers=self.zek_headers)
        self.assertEqual(code, 401)

        # 2. ZCI gateway rejects unauthenticated loopback, ZEKANS token, and cross-site Origin
        code, _ = self.get(self.zci_port, '/v1/models')
        self.assertEqual(code, 401)
        code, _ = self.post(self.zci_port, '/v1/chat/completions', {
            'model': sg.ZCI_ALIAS,
            'messages': [{'role': 'user', 'content': 'Merhaba'}],
        })
        self.assertEqual(code, 401)
        code, _ = self.post(self.zci_port, '/v1/chat/completions', {
            'model': sg.ZCI_ALIAS,
            'messages': [{'role': 'user', 'content': 'Merhaba'}],
        }, headers=self.zek_headers)
        self.assertEqual(code, 401)
        code, _ = self.post(self.zci_port, '/v1/chat/completions', {
            'model': sg.ZCI_ALIAS,
            'messages': [{'role': 'user', 'content': 'Merhaba'}],
        }, headers={'Authorization': 'Bearer local', 'Origin': 'https://evil.example'})
        self.assertEqual(code, 403)

        # 3. ZEKANS gateway rejects unauthenticated loopback and ZCI token
        code, _ = self.get(self.zek_port, '/v1/models')
        self.assertEqual(code, 401)
        code, _ = self.get(self.zek_port, '/props', headers=self.zci_headers)
        self.assertEqual(code, 401)
        code, _ = self.post(self.zek_port, '/v1/chat/completions', {
            'model': sg.ZEKANS_ALIAS,
            'messages': [{'role': 'user', 'content': 'Merhaba'}],
            'max_tokens': 768,
            'temperature': 0.2,
        }, headers=self.zci_headers)
        self.assertEqual(code, 401)

    def test_cross_gateway_alias_and_tool_rejection(self):
        # ZCI rejects ZEKANS model alias
        code, _ = self.post(self.zci_port, '/v1/chat/completions', {
            'model': sg.ZEKANS_ALIAS,
            'messages': [{'role': 'user', 'content': 'Merhaba'}],
        }, headers=self.zci_headers)
        self.assertEqual(code, 400)

        # ZEKANS rejects ZCI model alias
        code, _ = self.post(self.zek_port, '/v1/chat/completions', {
            'model': sg.ZCI_ALIAS,
            'messages': [{'role': 'user', 'content': 'Merhaba'}],
            'max_tokens': 768,
            'temperature': 0.2,
        }, headers=self.zek_headers)
        self.assertEqual(code, 400)

        # ZCI rejects tools/functions
        code, _ = self.post(self.zci_port, '/v1/chat/completions', {
            'model': sg.ZCI_ALIAS,
            'messages': [{'role': 'user', 'content': 'Merhaba'}],
            'tools': [{'type': 'function', 'function': {'name': 'sh'}}],
        }, headers=self.zci_headers)
        self.assertEqual(code, 400)

    def test_system_prompt_and_session_isolation(self):
        # 1. ZCI request
        code, res1 = self.post(self.zci_port, '/v1/chat/completions', {
            'model': sg.ZCI_ALIAS,
            'messages': [{'role': 'user', 'content': 'Gizli ZCI bilgisi: Mavi-Elma-42'}],
            'stream': False,
        }, headers=self.zci_headers)
        self.assertEqual(code, 200)
        zci_upstream = [b for p, b in self.engine.httpd.requests if p == '/v1/chat/completions'][-1]
        self.assertEqual(zci_upstream['messages'][0]['content'], 'ZCI SOHBET SISTEM TALIMATI')
        self.assertFalse(zci_upstream['cache_prompt'])

        # 2. ZEKANS request
        code, res2 = self.post(self.zek_port, '/v1/chat/completions', {
            'model': sg.ZEKANS_ALIAS,
            'messages': [
                {'role': 'system', 'content': 'ZEKANS KODLAMA SISTEM TALIMATI'},
                {'role': 'user', 'content': 'main.py oluştur'},
            ],
            'max_tokens': 768,
            'temperature': 0.2,
            'stream': False,
        }, headers=self.zek_headers)
        self.assertEqual(code, 200)
        zek_upstream = [b for p, b in self.engine.httpd.requests if p == '/v1/chat/completions'][-1]
        self.assertEqual(zek_upstream['messages'][0]['content'], 'ZEKANS KODLAMA SISTEM TALIMATI')
        self.assertNotIn('Mavi-Elma-42', json.dumps(zek_upstream))
        self.assertNotIn('ZCI SOHBET SISTEM TALIMATI', json.dumps(zek_upstream))
        self.assertFalse(zek_upstream['cache_prompt'])

    def test_concurrent_request_returns_429_busy_and_recovers(self):
        self.engine.httpd.delay = 0.5
        results = []

        def call_zci():
            results.append(self.post(self.zci_port, '/v1/chat/completions', {
                'model': sg.ZCI_ALIAS,
                'messages': [{'role': 'user', 'content': 'Sorgu 1'}],
                'stream': False,
            }, headers=self.zci_headers)[0])

        t = threading.Thread(target=call_zci)
        t.start()
        time.sleep(0.1)
        # Concurrent ZEKANS request must receive 429 Busy immediately without consuming its budget
        code_busy, body_busy = self.post(self.zek_port, '/v1/chat/completions', {
            'model': sg.ZEKANS_ALIAS,
            'messages': [{'role': 'user', 'content': 'Sorgu 2'}],
            'max_tokens': 768,
            'temperature': 0.2,
            'stream': False,
        }, headers=self.zek_headers)
        self.assertEqual(code_busy, 429)
        self.assertIn('meşgul', body_busy['detail'])
        self.assertEqual(body_busy.get('error', {}).get('code'), 'engine_busy')
        t.join(timeout=3)
        self.assertEqual(results, [200])

        # Subsequent ZEKANS request after slot release succeeds and still has its full 2-call budget
        self.engine.httpd.delay = 0.0
        for _ in range(2):
            code_ok, _ = self.post(self.zek_port, '/v1/chat/completions', {
                'model': sg.ZEKANS_ALIAS,
                'messages': [{'role': 'user', 'content': 'Sorgu 3'}],
                'max_tokens': 768,
                'temperature': 0.2,
                'stream': False,
            }, headers=self.zek_headers)
            self.assertEqual(code_ok, 200)

    def test_zekans_budget_non_resettable_and_process_bound_lifecycle(self):
        # 2 calls succeed (1 initial + 1 reflection)
        for _ in range(2):
            code, _ = self.post(self.zek_port, '/v1/chat/completions', {
                'model': sg.ZEKANS_ALIAS,
                'messages': [{'role': 'user', 'content': 'Deneme'}],
                'max_tokens': 768,
                'temperature': 0.2,
                'stream': False,
            }, headers=self.zek_headers)
            self.assertEqual(code, 200)
        # 3rd call in same task is rejected (reflection=1 cap)
        code, body_exhaust = self.post(self.zek_port, '/v1/chat/completions', {
            'model': sg.ZEKANS_ALIAS,
            'messages': [{'role': 'user', 'content': 'Fazla deneme'}],
            'max_tokens': 768,
            'temperature': 0.2,
            'stream': False,
        }, headers=self.zek_headers)
        self.assertEqual(code, 429)
        self.assertEqual(body_exhaust.get('error', {}).get('code'), 'budget_exhausted')

        # Old /zekans/new-task reset endpoint no longer exists (404)
        code, _ = self.post(self.zek_port, '/zekans/new-task', {})
        self.assertEqual(code, 404)

        # Attempting to re-bind the same process or bind another process while active task is alive fails (409)
        new_tok = os.urandom(32).hex()
        new_sha = hashlib.sha256(new_tok.encode()).hexdigest()
        code, _ = self.post(self.zek_port, '/zekans/bind-task', {'pid': os.getpid(), 'token_sha256': new_sha})
        self.assertEqual(code, 409)

        dummy = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(10)'])
        try:
            # While the first task session is still active, even a brand-new local process cannot bind or reset budget
            code, _ = self.post(self.zek_port, '/zekans/bind-task', {'pid': dummy.pid, 'token_sha256': new_sha})
            self.assertEqual(code, 409)

            # Once the first task finishes, its token becomes invalid (401)
            code, _ = self.post(self.zek_port, '/zekans/finish-task', {}, headers=self.zek_headers)
            self.assertEqual(code, 200)
            code, _ = self.post(self.zek_port, '/v1/chat/completions', {
                'model': sg.ZEKANS_ALIAS,
                'messages': [{'role': 'user', 'content': 'Kapalı görev'}],
                'max_tokens': 768,
                'temperature': 0.2,
                'stream': False,
            }, headers=self.zek_headers)
            self.assertEqual(code, 401)

            # Even after finishing, the old PID can never be re-bound (409)
            code, _ = self.post(self.zek_port, '/zekans/bind-task', {'pid': os.getpid(), 'token_sha256': new_sha})
            self.assertEqual(code, 409)

            # A genuinely new task process (dummy.pid) can now bind once and gets its own 2-call budget
            code, _ = self.post(self.zek_port, '/zekans/bind-task', {'pid': dummy.pid, 'token_sha256': new_sha})
            self.assertEqual(code, 200)
            code, _ = self.post(self.zek_port, '/v1/chat/completions', {
                'model': sg.ZEKANS_ALIAS,
                'messages': [{'role': 'user', 'content': 'Yeni görev'}],
                'max_tokens': 768,
                'temperature': 0.2,
                'stream': False,
            }, headers={'Authorization': f'Bearer {new_tok}'})
            self.assertEqual(code, 200)
        finally:
            dummy.terminate()
            dummy.wait(timeout=3)

    def test_portable_user_paths_have_no_hardcoded_home(self):
        source = (HERE / 'shared_gemma.py').read_text()
        self.assertNotIn('/home/' + 'Zevor', source)
        expected_root = Path.home() / '.local' / 'share' / 'zci'
        self.assertEqual(sg.ZCI_HOME, expected_root)
        self.assertEqual(sg.DEFAULT_AGENT_BASE, expected_root / 'agent')
        self.assertTrue(str(sg.DEFAULT_ENGINE).startswith(str(expected_root)))
        self.assertTrue(str(sg.DEFAULT_MODEL).startswith(str(expected_root)))


if __name__ == '__main__':
    unittest.main()

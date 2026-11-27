import copy
import io
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from dataclasses import replace
from functools import partial
import urllib.error
import urllib.request
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parent))
import zci_chat as z

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

class FakeModel(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def send_json(self, data):
        raw = json.dumps(data).encode(); self.send_response(200)
        self.send_header('Content-Length', str(len(raw))); self.end_headers(); self.wfile.write(raw)
    def do_GET(self):
        if self.path == '/props':
            self.send_json({'model_alias': self.server.alias, 'model_path': str(self.server.model),
                'chat_template': self.server.template, 'default_generation_settings': {'n_ctx': 32768}})
        else: self.send_json({'data': [{'id': self.server.alias}]})
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.calls.append((self.path, body))
        if self.path == '/apply-template': return self.send_json({'prompt': json.dumps(body)})
        if self.path == '/tokenize': return self.send_json({'tokens': [1]*self.server.count})
        text = body['messages'][-1]['content']
        reason = 'length' if text == 'cutoff' else 'stop'
        answer = '' if text == 'empty' else 'yarım' if text == 'cutoff' else 'Yanıt.'
        usage = {'prompt_tokens': self.server.count, 'completion_tokens': 2}
        if not body.get('stream'):
            return self.send_json({'id': 'fake', 'choices': [{'message': {'content': answer,
                'reasoning_content': 'HIDDEN'}, 'finish_reason': reason}], 'usage': usage})
        self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.end_headers()
        def send(delta, finish=None, u=None):
            data = {'choices': [{'delta': delta, 'finish_reason': finish}]}
            if u: data['usage'] = u
            self.wfile.write(b'data: '+json.dumps(data).encode()+b'\n\n'); self.wfile.flush()
        try:
            send({'reasoning_content': 'HIDDEN'})
            if text == 'unsafe':
                send({'content': '<thi'}); send({'content': 'nk>HIDDEN</think>answer'})
            elif text == 'slow':
                for _ in range(100):
                    send({'content': 'x'}); time.sleep(.02)
            else: send({'content': answer})
            if text == 'disconnect': return
            send({}, reason, usage)
            self.wfile.write(b'data: [DONE]\n\n'); self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError): pass

class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='zci-chat-unit-')
        p = Path(self.tmp.name)
        self.model = p/'model.gguf'; self.model.write_bytes(b'GGUFfixture')
        self.lease_patch = patch.object(z, 'ModelLease', partial(z.ModelLease,
            runtime_dir=p, min_available_bytes=1))
        self.lease_patch.start()
        self.system = p/'system.txt'; self.system.write_text('ZCI Türkçe')
        self.fake = ThreadingHTTPServer(('127.0.0.1', 0), FakeModel)
        self.fake.daemon_threads = True; self.fake.model = self.model; self.fake.alias = z.ALIAS
        self.fake.template = 'template'; self.fake.count = 7; self.fake.calls = []
        self.thread = threading.Thread(target=self.fake.serve_forever, daemon=True); self.thread.start()
        self.config = z.Config(Path(sys.executable), self.model, self.system,
                               self.fake.server_port, port(), model_sha256=z.file_hash(self.model), timeout=5)
        self.gateway = z.Gateway(self.config).start()
        self.url = f'http://127.0.0.1:{self.config.gateway_port}'
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def tearDown(self):
        self.gateway.close(); self.fake.shutdown(); self.fake.server_close()
        self.thread.join(2); self.lease_patch.stop(); self.tmp.cleanup()
    def body(self, text='ok', stream=False):
        return {'model': z.ALIAS, 'messages': [{'role': 'user', 'content': text}], 'stream': stream}
    def request(self, body):
        req = urllib.request.Request(self.url+'/v1/chat/completions', data=json.dumps(body).encode(),
                                     headers={'Content-Type': 'application/json'})
        return self.opener.open(req, timeout=10)
    def test_json_thoughts_removed_and_count_matches(self):
        with self.request(self.body()) as r: data = json.load(r)
        self.assertEqual(data['choices'][0]['message']['content'], 'Yanıt.')
        self.assertNotIn('HIDDEN', json.dumps(data)); self.assertEqual(data['zci_status'], 'completed')
    def test_sse_thoughts_removed(self):
        with self.request(self.body(stream=True)) as r: data = r.read().decode()
        self.assertNotIn('HIDDEN', data); self.assertIn('[DONE]', data); self.assertIn('completed', data)
    def test_cutoff_json_visible_and_not_complete(self):
        with self.request(self.body('cutoff')) as r: data = json.load(r)
        self.assertIn('tamamlanamadı', data['choices'][0]['message']['content'])
        self.assertEqual(data['zci_status'], 'incomplete')
    def test_empty_json_has_visible_notice(self):
        with self.request(self.body('empty')) as r: data = json.load(r)
        self.assertIn('nihai yanıt oluşmadı', data['choices'][0]['message']['content'])
        self.assertEqual(data['zci_status'], 'incomplete')
    def test_sse_interruption_is_visible(self):
        with self.request(self.body('disconnect', True)) as r: data = r.read().decode()
        self.assertIn('Bağlantı kesildi', data); self.assertIn('incomplete', data)
    def test_split_reasoning_tag_never_leaks(self):
        with self.request(self.body('unsafe', True)) as r: data = r.read().decode()
        self.assertNotIn('HIDDEN', data); self.assertNotIn('<think>', data); self.assertIn('incomplete', data)
    def test_wrong_model_never_reaches_upstream(self):
        body = self.body(); body['model'] = 'zci-qwen3-1.7b'
        with self.assertRaises(urllib.error.HTTPError) as caught: self.request(body)
        self.assertEqual(caught.exception.code, 400); caught.exception.close()
        self.assertFalse(self.fake.calls)
    def test_wrong_physical_model_blocks(self):
        self.fake.model = self.model.parent/'other.gguf'
        with self.assertRaises(urllib.error.HTTPError) as caught: self.request(self.body())
        self.assertEqual(caught.exception.code, 503); caught.exception.close()
    def test_boundary_and_no_input_rewriting(self):
        b = self.body(); b['messages'].insert(0, {'role': 'system', 'content': 'ZCI Türkçe\ncustom'})
        before = copy.deepcopy(b); self.fake.count = 32000
        payload, count, _, _ = self.gateway.backend.prepare(b)
        self.assertEqual(payload['messages'][0]['content'], 'ZCI Türkçe\ncustom')
        self.assertEqual(count, 32000); self.assertEqual(b, before)
        self.fake.count = 32001
        with self.assertRaises(z.ChatError): self.gateway.backend.prepare(b)
        self.assertFalse(any(path == '/v1/chat/completions' for path, body in self.fake.calls))
    def test_five_turn_history_reaches_json_and_sse_unchanged(self):
        history = []
        for turn in range(4):
            history.extend([{'role': 'user', 'content': f'Soru {turn}: iş arkadaşım'},
                            {'role': 'assistant', 'content': f'Yanıt {turn}'}])
        history.append({'role': 'user', 'content': 'Az önce kimi söyledim?'})
        for stream in (False, True):
            with self.subTest(stream=stream):
                body = self.body(stream=stream)
                body['messages'] = [{'role': 'system', 'content': 'ZCI Türkçe\nZCI Türkçe'}] + copy.deepcopy(history)
                before = copy.deepcopy(body)
                with self.request(body) as response:
                    response.read()
                sent = [data for path, data in self.fake.calls if path == '/v1/chat/completions'][-1]
                rendered = [data for path, data in self.fake.calls if path == '/apply-template'][-1]
                expected = [{'role': 'system', 'content': 'ZCI Türkçe'}] + history
                self.assertEqual(sent['messages'], expected)
                self.assertEqual(rendered['messages'], expected)
                self.assertEqual(body, before)
    def test_profiles_share_render_and_generation_flag(self):
        body = self.body(); body['model'] += '-thinking'
        payload, _, _, _ = self.gateway.backend.prepare(body)
        render = next(b for p, b in self.fake.calls if p == '/apply-template')
        self.assertTrue(payload['chat_template_kwargs']['enable_thinking'])
        self.assertEqual(render['chat_template_kwargs'], payload['chat_template_kwargs'])
    def test_supported_ui_sampling_is_honored(self):
        body = self.body(); body.update(temperature=.8, top_p=.9, top_k=12, seed=42, stop=['SON'])
        payload, _, _, _ = self.gateway.backend.prepare(body)
        for key in ['temperature', 'top_p', 'top_k', 'seed', 'stop']:
            self.assertEqual(payload[key], body[key])
        for key, value in [('temperature', True), ('top_p', 0), ('top_k', 1.5),
                           ('seed', '7'), ('stop', [False]), ('temperature', float('nan'))]:
            bad = self.body(); bad[key] = value
            with self.assertRaises(z.ChatError): self.gateway.backend.prepare(bad)
    def test_template_switch_rejected(self):
        self.gateway.backend.identity(); self.fake.template = 'changed'
        with self.assertRaises(z.ChatError): self.gateway.backend.identity()
    def test_invalid_stream_tools_and_budget(self):
        for key, value in [('stream', 'false'), ('tools', []), ('max_tokens', 769), ('max_tokens', True)]:
            body = self.body(); body[key] = value
            with self.assertRaises(z.ChatError): self.gateway.backend.prepare(body)
    def test_client_cancel_releases_upstream(self):
        response = self.request(self.body('slow', True))
        response.readline(); response.close()
        deadline = time.monotonic()+4
        while time.monotonic() < deadline:
            if not self.gateway.backend.active and not self.gateway.backend.turn.locked(): break
            time.sleep(.05)
        self.assertFalse(self.gateway.backend.active)
        self.assertFalse(self.gateway.backend.turn.locked())
    def test_reserved_ports_and_no_autodouble_start(self):
        for p in [8087, 8088, 8089]:
            cfg = z.Config(Path(sys.executable), self.model, self.system, p, self.config.gateway_port)
            with self.assertRaises(ValueError): cfg.validate()
        with patch.object(z, 'available_ram', return_value=1024**3), patch.object(z.subprocess, 'Popen') as popen:
            with self.assertRaises(z.ChatError): z.Engine(self.config, self.model.parent).__enter__()
            popen.assert_not_called()
    def test_live_listener_not_reused(self):
        with self.assertRaises(OSError): z.Gateway(self.config)
    def test_upstream_http_error_response_is_closed(self):
        payload = io.BytesIO(b'not ready')
        error = urllib.error.HTTPError('http://127.0.0.1', 503, 'not ready', {}, payload)
        with patch.object(self.gateway.backend.opener, 'open', side_effect=error):
            with self.assertRaises(z.ChatError): self.gateway.backend.api('/props')
        self.assertTrue(payload.closed)
        self.assertFalse(self.gateway.backend.active)
    def test_sigterm_during_startup_reaps_owned_engine(self):
        root = self.model.parent; pidfile = root/'child.pid'
        engine = root/'fake-engine'
        engine.write_text('#!'+sys.executable+'\nimport os,time\nfrom pathlib import Path\n'
                          +'Path('+repr(str(pidfile))+').write_text(str(os.getpid()))\n'
                          +'while True: time.sleep(.1)\n')
        engine.chmod(0o700)
        config = root/'config.json'
        config.write_text(json.dumps({'engine': str(engine), 'model': str(self.model),
            'system_file': str(self.system), 'engine_port': port(), 'gateway_port': port(),
            'model_sha256': z.file_hash(self.model), 'timeout': 2}))
        launch = ('import sys; from functools import partial; '
                  +'sys.path.insert(0, '+repr(str(Path(z.__file__).parent))+'); import zci_chat as z; '
                  +'z.ModelLease=partial(z.ModelLease,runtime_dir='+repr(str(root))+',min_available_bytes=1); '
                  +'sys.argv=["zci_chat.py","--config",'+repr(str(config))+']; z.main()')
        child = subprocess.Popen([sys.executable, '-I', '-B', '-c', launch],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        owned = None
        try:
            deadline = time.monotonic()+8
            while not pidfile.exists() and time.monotonic() < deadline: time.sleep(.05)
            self.assertTrue(pidfile.exists(), 'owned engine did not start')
            owned = int(pidfile.read_text())
            child.send_signal(signal.SIGTERM)
            self.assertEqual(child.wait(timeout=8), 0)
            self.assertFalse(Path(f'/proc/{owned}').exists(), 'owned engine leaked')
        finally:
            if child.poll() is None: child.kill(); child.wait(3)
            if owned is not None and Path(f'/proc/{owned}').exists():
                try: os.killpg(owned, signal.SIGKILL)
                except ProcessLookupError: pass
    def test_numeric_unknown_regression_fixture_is_rejected(self):
        fixture = json.loads(Path(__file__).with_name('unknown_regression.json').read_text())
        self.assertIn('12 bayt', fixture['known_bad_answer']); self.assertFalse(fixture['semantic_acceptance'])
    def test_missing_model_digest_is_not_silently_accepted(self):
        cfg = z.Config(Path(sys.executable), self.model, self.system, self.fake.server_port, port())
        with self.assertRaisesRegex(ValueError, 'SHA-256'): cfg.validate()
    def gemma(self):
        self.gateway.close()
        self.config = replace(self.config, profile='gemma4-e4b')
        self.fake.alias = self.config.alias
        self.fake.template = '<|turn>system\n{{ messages }}<turn|>\n<|turn>model\n'
        self.gateway = z.Gateway(self.config).start()
    def test_gemma_fixed_identity_sampling_and_no_unvalidated_thinking_alias(self):
        self.gemma()
        body = self.body(); body['model'] = self.config.alias
        payload, _, _, _ = self.gateway.backend.prepare(body)
        self.assertEqual(payload['model'], 'zci-chat-gemma4-e4b')
        self.assertEqual(payload['top_k'], 64); self.assertEqual(payload['min_p'], .05)
        self.assertFalse(payload['chat_template_kwargs']['enable_thinking'])
        for wrong in (z.ALIAS, self.config.alias+'-thinking'):
            body['model'] = wrong
            with self.assertRaises(z.ChatError): self.gateway.backend.prepare(body)
        with self.opener.open(self.url+'/v1/models') as response: models=json.load(response)
        self.assertEqual([m['id'] for m in models['data']], [self.config.alias])
    def test_gemma_rejects_wrong_native_template_before_inference(self):
        self.gemma(); self.fake.template = 'chatml'
        with self.assertRaises(z.ChatError): self.gateway.backend.identity()
        self.assertFalse(self.fake.calls)
    def test_reasoning_control_markers_are_blocked_at_every_sse_split(self):
        for marker in z.TAGS:
            for split in range(1, len(marker)):
                with self.subTest(marker=marker, split=split):
                    safe = z.SafeText()
                    self.assertEqual(safe.feed(marker[:split]), '')
                    with self.assertRaises(z.ChatError): safe.feed(marker[split:]+'SECRET')
        safe = z.SafeText()
        self.assertEqual(safe.feed('Türkçe son yanıt.'), 'Türkçe son yanıt.')
        safe.finish()
    def test_lease_released_after_pre_spawn_integrity_failure(self):
        bad=replace(self.config, model_sha256='0'*64)
        with self.assertRaises(z.ChatError): z.Engine(bad, self.model.parent).__enter__()
        with z.ModelLease('probe'): pass
    def test_ram_monitor_failure_stops_only_owned_engine_and_releases_lease(self):
        root=self.model.parent; executable=root/'sleep-engine'
        executable.write_text('#!'+sys.executable+'\nimport time\nwhile True: time.sleep(.1)\n')
        executable.chmod(0o700)
        config=replace(self.config,engine=executable,engine_port=port())
        with patch.object(z.Backend,'identity',return_value='fixture'), patch.object(
                z,'available_ram',side_effect=[16*1024**3,OSError('unreadable RAM')]):
            with z.Engine(config,root) as engine:
                deadline=time.monotonic()+4
                while engine.process.poll() is None and time.monotonic()<deadline: time.sleep(.05)
                self.assertIsNotNone(engine.process.poll())
                self.assertEqual(engine.abort,'RAM izleme doğrulanamadı')
        with z.ModelLease('after-monitor-error'): pass

    def test_portable_paths_and_default_temp_dir_cleanup(self):
        base = Path(__file__).resolve().parent
        for name in ('zci_chat.py', 'live_acceptance.py', 'gemma_webui_acceptance.py',
                     'profile_acceptance.py', 'conversation_acceptance.py'):
            self.assertNotIn('/home/' + 'Zevor', (base / name).read_text(), name)
        created = []
        orig_mkdtemp = tempfile.mkdtemp
        def tracking_mkdtemp(*args, **kwargs):
            path = orig_mkdtemp(*args, **kwargs)
            created.append(Path(path))
            return path
        cfg_file = self.model.parent / 'cleanup-config.json'
        cfg_file.write_text(json.dumps({
            'engine': str(self.config.engine), 'model': str(self.model),
            'system_file': str(self.system), 'engine_port': self.config.engine_port,
            'gateway_port': port(), 'model_sha256': self.config.model_sha256, 'timeout': 2,
        }))
        with patch.object(z.tempfile, 'mkdtemp', side_effect=tracking_mkdtemp), \
             patch.object(z, 'Engine', side_effect=KeyboardInterrupt), \
             patch.object(sys, 'argv', ['zci_chat.py', '--config', str(cfg_file)]):
            z.main()
        self.assertEqual(len(created), 1)
        self.assertFalse(created[0].exists())

if __name__ == '__main__': unittest.main(verbosity=2)

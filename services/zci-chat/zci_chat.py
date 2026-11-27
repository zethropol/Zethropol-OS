"""User-owned, loopback-only ZCI chat gateway. No install or 1.7B startup."""
import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'local-inference'))
from inference_guard import BusyError, ModelLease, ResourceError

CONTEXT = 32768
OUTPUT = 768
ALIAS = 'zci-chat-qwen3-4b'
PROFILES = {
    'qwen3-4b': {'alias': ALIAS, 'top_k': 20, 'min_p': 0, 'thinking': True},
    'gemma4-e4b': {'alias': 'zci-chat-gemma4-e4b', 'top_k': 64, 'min_p': .05, 'thinking': False},
}
WARNING = '\n\n[ZCI: Yanıt tamamlanamadı — çıktı sınırına ulaşıldı.]'
INTERRUPTED = '\n\n[ZCI: Bağlantı kesildi; yanıt tamamlanamadı.]'
EMPTY = '[ZCI: Kullanıcıya aktarılacak nihai yanıt oluşmadı.]'
TAGS = ('<think>', '</think>', '<|channel>', '<channel|>', '<|think|>', '<|turn>', '<turn|>')

class ChatError(Exception):
    def __init__(self, message, status=503):
        super().__init__(message)
        self.status = status

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        raise ChatError('ZCI model bağlantısı başka adrese yönlendirilemez.')

@dataclass(frozen=True)
class Config:
    engine: Path
    model: Path
    system_file: Path
    engine_port: int = 18087
    gateway_port: int = 18086
    model_sha256: str = ''
    timeout: float = 180
    profile: str = 'qwen3-4b'

    @classmethod
    def load(cls, path):
        path = Path(path).resolve()
        data = json.loads(path.read_text())
        for key in ('engine', 'model', 'system_file'):
            p = Path(data[key]).expanduser()
            data[key] = (path.parent/p).resolve()
        obj = cls(**data)
        obj.validate()
        return obj

    def validate(self):
        if self.profile not in PROFILES:
            raise ValueError('Unknown fixed ZCI chat profile')
        for port in (self.engine_port, self.gateway_port):
            if type(port) is not int or not 1024 <= port <= 65535 or port in {8087, 8088, 8089}:
                raise ValueError('Use separate unprivileged loopback ports; 8087/8088/8089 are reserved.')
        if self.engine_port == self.gateway_port or not 1 <= self.timeout <= 300:
            raise ValueError('Invalid port or request deadline')
        for p in (self.engine, self.model, self.system_file):
            if not p.is_file():
                raise ValueError(f'Required local file missing: {p.name}')
        if (not isinstance(self.model_sha256, str) or len(self.model_sha256) != 64
                or any(c not in '0123456789abcdef' for c in self.model_sha256)):
            raise ValueError('An explicit model SHA-256 is required')

    @property
    def upstream(self):
        return f'http://127.0.0.1:{self.engine_port}'

    @property
    def alias(self):
        return PROFILES[self.profile]['alias']

    @property
    def aliases(self):
        return (self.alias, self.alias+'-thinking') if PROFILES[self.profile]['thinking'] else (self.alias,)

def file_hash(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            h.update(block)
    return h.hexdigest()

def available_ram():
    return int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines()
                    if line.startswith('MemAvailable:')))*1024

class SafeText:
    """Hold possible reasoning-tag prefixes across SSE boundaries."""
    def __init__(self):
        self.pending = ''

    def feed(self, text):
        if not isinstance(text, str):
            raise ChatError('Geçersiz model metni.', 502)
        self.pending += text
        out = ''
        while self.pending:
            if any(self.pending.startswith(tag) for tag in TAGS):
                raise ChatError('Ayrıştırılmamış düşünme metni gösterilmedi.', 502)
            if any(tag.startswith(self.pending) for tag in TAGS):
                break
            out += self.pending[0]
            self.pending = self.pending[1:]
        return out

    def finish(self):
        if self.pending:
            raise ChatError('Tamamlanmamış düşünme etiketi gösterilmedi.', 502)

def finish_text(text, reason):
    if reason != 'stop':
        return WARNING, 'incomplete'
    if not text.strip():
        return EMPTY, 'incomplete'
    return '', 'completed'

class Backend:
    def __init__(self, config):
        self.config = config
        self.system = config.system_file.read_text().strip()
        if not self.system:
            raise ValueError('Empty system prompt')
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        self.template = None
        self.active = set()
        self.lock = threading.Lock()
        self.turn = threading.Lock()

    @contextmanager
    def response(self, path, body=None):
        request = urllib.request.Request(self.config.upstream+path,
            data=json.dumps(body, ensure_ascii=False).encode() if body is not None else None,
            headers={'Content-Type': 'application/json'})
        try:
            response = self.opener.open(request, timeout=self.config.timeout)
        except urllib.error.HTTPError as error:
            error.close()
            raise ChatError('ZCI 4B modeli isteği reddetti; başka modele geçilmedi.', 502) from error
        except (OSError, ValueError) as error:
            raise ChatError('ZCI 4B modeliyle bağlantı kurulamadı; başka modele geçilmedi.') from error
        with self.lock:
            self.active.add(response)
        try:
            yield response
        finally:
            response.close()
            with self.lock:
                self.active.discard(response)

    def close(self):
        with self.lock:
            active = list(self.active)
        for response in active:
            try:
                response.fp.raw._sock.shutdown(socket.SHUT_RDWR)
            except (AttributeError, OSError):
                pass
            response.close()

    def api(self, path, body=None):
        try:
            with self.response(path, body) as response:
                raw = response.read(2*1024*1024+1)
            if len(raw) > 2*1024*1024:
                raise ValueError('oversized')
            data = json.loads(raw)
            if not isinstance(data, dict):
                raise ValueError('object required')
            return data
        except (OSError, ValueError) as error:
            raise ChatError('ZCI modelinden geçerli yanıt alınamadı.', 502) from error

    def identity(self):
        try:
            props = self.api('/props')
            ids = [m['id'] for m in self.api('/v1/models')['data']]
            if (ids != [self.config.alias] or props['model_alias'] != self.config.alias
                    or props['model_path'] != str(self.config.model)
                    or props['default_generation_settings']['n_ctx'] != CONTEXT):
                raise ValueError('wrong identity')
            template = hashlib.sha256(props['chat_template'].encode()).hexdigest()
            if self.config.profile == 'gemma4-e4b' and not all(
                    tag in props['chat_template'] for tag in ('<|turn>', '<turn|>')):
                raise ValueError('Gemma 4 native template required')
            if self.template is not None and self.template != template:
                raise ValueError('changed template')
            self.template = template
            return template
        except (KeyError, TypeError, ValueError) as error:
            raise ChatError('ZCI model kimliği veya bağlamı uyuşmuyor; istek durduruldu.') from error

    def prepare(self, body):
        if not isinstance(body, dict):
            raise ChatError('Geçersiz sohbet isteği.', 400)
        model = body.get('model')
        if model not in self.config.aliases:
            raise ChatError('Bu bağlantı yalnız seçili ZCI sohbet profilini kabul eder.', 400)
        allowed = {'model', 'messages', 'stream', 'stream_options', 'max_tokens',
                   'temperature', 'top_p', 'top_k', 'min_p', 'seed', 'stop',
                   'frequency_penalty', 'presence_penalty'}
        if set(body)-allowed:
            raise ChatError('Bu sohbet bağlantısı araç veya özel yürütme alanlarını desteklemez.', 400)
        stream = body.get('stream', False)
        output = body.get('max_tokens', OUTPUT)
        if type(stream) is not bool or type(output) is not int or not 1 <= output <= OUTPUT:
            raise ChatError('Geçersiz akış veya çıktı bütçesi; en çok 768 token.', 400)
        messages = []
        if not isinstance(body.get('messages'), list):
            raise ChatError('Metin mesajları gereklidir.', 400)
        for message in body['messages']:
            if (not isinstance(message, dict) or message.get('role') not in {'system', 'user', 'assistant'}
                    or not isinstance(message.get('content'), str)):
                raise ChatError('Yalnız metin mesajları desteklenir.', 400)
            messages.append({'role': message['role'], 'content': message['content']})
        if not messages:
            raise ChatError('Boş sohbet gönderilemez.', 400)
        if messages[0]['role'] == 'system':
            text = messages[0]['content']
            while text == self.system or text.startswith(self.system+'\n'):
                text = text[len(self.system):].removeprefix('\n')
            messages = ([{'role': 'system', 'content': text}] if text else [])+messages[1:]
        if messages and messages[0]['role'] == 'system':
            messages[0]['content'] = self.system+'\n'+messages[0]['content']
        else:
            messages.insert(0, {'role': 'system', 'content': self.system})
        identity = self.identity()
        kwargs = {'enable_thinking': model.endswith('-thinking')}
        try:
            prompt = self.api('/apply-template', {'messages': messages, 'chat_template_kwargs': kwargs})['prompt']
            if not isinstance(prompt, str):
                raise ValueError('bad template')
            tokens = self.api('/tokenize', {'content': prompt, 'add_special': True, 'parse_special': True})['tokens']
            if not isinstance(tokens, list) or any(type(t) is not int or t < 0 for t in tokens):
                raise ValueError('bad tokens')
        except (KeyError, TypeError, ValueError) as error:
            raise ChatError('ZCI token sayımı doğrulanamadı.', 502) from error
        if len(tokens)+output > CONTEXT:
            raise ChatError('ZCI bağlamı dolu: giriş ve ayrılan çıktı 32768 tokenı aşıyor.', 400)
        if self.identity() != identity:
            raise ChatError('Model istek hazırlanırken değişti.')
        # Profiles provide defaults; supported UI sampling controls remain explicit.
        profile = PROFILES[self.config.profile]
        payload = {'model': self.config.alias, 'messages': messages, 'stream': stream,
                   'max_tokens': output, 'temperature': .2 if not kwargs['enable_thinking'] else .6,
                   'top_p': .95, 'top_k': profile['top_k'], 'min_p': profile['min_p'], 'seed': 7,
                   'cache_prompt': False, 'chat_template_kwargs': kwargs, 'reasoning_format': 'auto'}
        for key, low, high in [('temperature', 0, 2), ('top_p', .000001, 1),
                               ('min_p', 0, 1), ('frequency_penalty', -2, 2),
                               ('presence_penalty', -2, 2)]:
            if key in body:
                value = body[key]
                if type(value) not in {int, float} or not low <= value <= high:
                    raise ChatError('Geçersiz üretim parametresi: '+key, 400)
                payload[key] = value
        for key, low, high in [('top_k', 0, 1000), ('seed', -1, 4294967295)]:
            if key in body:
                value = body[key]
                if type(value) is not int or not low <= value <= high:
                    raise ChatError('Geçersiz üretim parametresi: '+key, 400)
                payload[key] = value
        if body.get('stop') is not None:
            stops = [body['stop']] if isinstance(body['stop'], str) else body['stop']
            if not isinstance(stops, list) or len(stops) > 4 or any(not isinstance(s, str) or len(s) > 128 for s in stops):
                raise ChatError('Geçersiz durdurma dizisi.', 400)
            payload['stop'] = stops
        if stream:
            payload['stream_options'] = {'include_usage': True}
        return payload, len(tokens), identity, model

    def json_reply(self, payload, count, identity, model):
        result = self.api('/v1/chat/completions', payload)
        try:
            if self.identity() != identity or result['usage']['prompt_tokens'] != count:
                raise ValueError('identity/count mismatch')
            choice = result['choices'][0]
            content = choice['message'].get('content') or ''
            safe = SafeText()
            text = safe.feed(content)
            safe.finish()
            note, status = finish_text(text, choice.get('finish_reason'))
            result['choices'] = [{'index': 0, 'message': {'role': 'assistant', 'content': text+note},
                                  'finish_reason': choice.get('finish_reason')}]
            result['model'] = model
            result['zci_status'] = status
            return result
        except (KeyError, TypeError, ValueError) as error:
            raise ChatError('ZCI yanıtı doğrulanamadı.', 502) from error

class Gateway(ThreadingHTTPServer):
    daemon_threads = True
    # Reuse TIME_WAIT addresses after an owned shutdown; never enable SO_REUSEPORT.
    allow_reuse_address = True
    def __init__(self, config):
        self.backend = Backend(config)
        super().__init__(('127.0.0.1', config.gateway_port), Handler)
        self.thread = None
    def start(self):
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()
        return self
    def close(self):
        self.backend.close()
        if self.thread:
            self.shutdown()
            self.thread.join(5)
        self.server_close()

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass  # Never log user prompts or authentication headers.
    def json(self, status, data):
        raw = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)
    def do_GET(self):
        try:
            if self.path not in {'/health', '/v1/models'}:
                return self.json(404, {'detail': 'Not found'})
            self.server.backend.identity()
            if self.path == '/health':
                return self.json(200, {'status': 'ok'})
            self.json(200, {'object': 'list', 'data': [{'id': name, 'object': 'model',
                'owned_by': 'zci'} for name in self.server.backend.config.aliases]})
        except ChatError as error:
            self.json(error.status, {'detail': str(error)})
    def do_POST(self):
        backend = self.server.backend
        if self.path != '/v1/chat/completions':
            return self.json(404, {'detail': 'Not found'})
        if not backend.turn.acquire(blocking=False):
            return self.json(429, {'detail': 'ZCI başka bir yanıt üretiyor; yeniden deneyin.'})
        try:
            self.connection.settimeout(10)
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 1048576:
                raise ChatError('İstek boyutu geçersiz.', 400)
            body = json.loads(self.rfile.read(size))
            payload, count, identity, model = backend.prepare(body)
            if payload['stream']:
                self.stream(payload, count, identity, model)
            else:
                self.json(200, backend.json_reply(payload, count, identity, model))
        except ChatError as error:
            self.json(error.status, {'detail': str(error)})
        except (ValueError, TypeError, socket.timeout):
            self.json(400, {'detail': 'Geçersiz veya tamamlanmamış ZCI isteği.'})
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            backend.turn.release()
    def stream(self, payload, count, identity, model):
        backend = self.server.backend
        # Open upstream before emitting headers so connection failures remain HTTP errors.
        with backend.response('/v1/chat/completions', payload) as upstream:
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream; charset=utf-8')
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()
            def event(content=None, finish=None, usage=None, status=None):
                packet = {'id': 'zci-chat-stream', 'object': 'chat.completion.chunk', 'model': model,
                          'choices': [{'index': 0, 'delta': {'content': content} if content is not None else {},
                                       'finish_reason': finish}]}
                if usage: packet['usage'] = usage
                if status: packet['zci_status'] = status
                self.wfile.write(b'data: '+json.dumps(packet, ensure_ascii=False).encode()+b'\n\n')
                self.wfile.flush()
            safe = SafeText(); text = ''; finish = None; usage = None; done = False
            deadline = time.monotonic()+backend.config.timeout
            try:
                for line in upstream:
                    if time.monotonic() > deadline or len(line) > 131072:
                        raise ChatError('Akış süresi veya boyutu aşıldı.', 502)
                    if not line.startswith(b'data: '): continue
                    raw = line[6:].strip()
                    if raw == b'[DONE]': done = True; break
                    data = json.loads(raw)
                    if data.get('error'): raise ChatError('Model akışı başarısız.', 502)
                    usage = data.get('usage') or usage
                    for choice in data.get('choices', []):
                        content = choice.get('delta', {}).get('content') or ''
                        clean = safe.feed(content)
                        if clean:
                            text += clean
                            event(clean)
                        else:
                            # Probe downstream cancellation during hidden reasoning too.
                            self.wfile.write(b': zci-wait\n\n'); self.wfile.flush()
                        finish = choice.get('finish_reason') or finish
                safe.finish()
                if not done or not finish or not usage or usage['prompt_tokens'] != count:
                    raise ChatError('Model akışı tamamlanmadı.', 502)
                if backend.identity() != identity:
                    raise ChatError('Akış sırasında model kimliği değişti.')
                note, status = finish_text(text, finish)
                if note: event(note)
                event(finish=finish, usage=usage, status=status)
            except (BrokenPipeError, ConnectionResetError):
                return  # Closing this context cancels only the owned upstream request.
            except (ChatError, OSError, ValueError, KeyError, TypeError):
                event(INTERRUPTED)
                event(finish='length', status='incomplete')
            self.wfile.write(b'data: [DONE]\n\n'); self.wfile.flush()

class Engine:
    """Own exactly one chat process; never reuse, stop or start the 8087 model."""
    def __init__(self, config, work):
        self.config = config; self.work = Path(work)
        self.process = None; self.stop = threading.Event(); self.thread = None
        self.log = None; self.abort = None
        self.lease = None
    def __enter__(self):
        try:
            self.config.validate()
            self.lease = ModelLease('zci-'+self.config.profile)
            self.lease.acquire()
            return self._start()
        except (BusyError, ResourceError) as error:
            self.close()
            raise ChatError(str(error)) from error
        except BaseException:
            self.close()
            raise
    def _start(self):
        c = self.config; c.validate()
        if available_ram() < 12*1024**3:
            raise ChatError('4B başlatılmadı: kullanılabilir RAM en az 12 GiB olmalı.')
        if file_hash(c.model) != c.model_sha256:
            raise ChatError('4B model dosyasının SHA-256 değeri uyuşmuyor.')
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(('127.0.0.1', c.engine_port))
        argv = [str(c.engine), '--model', str(c.model), '--alias', c.alias,
                '--host', '127.0.0.1', '--port', str(c.engine_port), '--device', 'none',
                '--n-gpu-layers', '0', '--no-op-offload', '--threads', '6', '--threads-batch', '6',
                '--threads-http', '2', '--ctx-size', str(CONTEXT), '--parallel', '1',
                '--batch-size', '512', '--ubatch-size', '128', '--offline', '--no-webui',
                '--no-agent', '--no-ui-mcp-proxy', '--cors-origins', 'localhost',
                '--jinja', '--reasoning', 'off', '--no-context-shift']
        try:
            self.log = (self.work/'engine.log').open('w')
            self.process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=self.log,
                stderr=subprocess.STDOUT, start_new_session=True,
                env={'HOME': str(self.work), 'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'OMP_NUM_THREADS': '6'})
            self.lease.attach_process(self.process.pid)
            def monitor():
                while not self.stop.wait(.5):
                    if self.process.poll() is not None: break
                    try:
                        status = Path(f'/proc/{self.process.pid}/status').read_text()
                        rss = int(next(s.split()[1] for s in status.splitlines() if s.startswith('VmRSS:')))*1024
                        if available_ram() < 6*1024**3 or rss > 18*1024**3:
                            self.abort = 'RAM koruması'
                            self.process.terminate(); break
                    except (OSError, StopIteration, ValueError):
                        if self.process.poll() is None:
                            self.abort = 'RAM izleme doğrulanamadı'
                            try: self.process.terminate()
                            except ProcessLookupError: pass
                        break
            self.thread = threading.Thread(target=monitor, daemon=True); self.thread.start()
            backend = Backend(c); deadline = time.monotonic()+120
            while time.monotonic() < deadline:
                if self.process.poll() is not None: raise ChatError('4B model süreci durdu.')
                try:
                    backend.identity()
                    return self
                except ChatError: time.sleep(.2)
            raise ChatError('4B başlatma süresi aşıldı.')
        except BaseException:
            self.close(); raise
    def close(self):
        self.stop.set()
        if self.process and self.process.poll() is None:
            try: os.killpg(self.process.pid, signal.SIGTERM)
            except ProcessLookupError: pass
            try: self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                try: os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                self.process.wait(timeout=5)
        if self.thread: self.thread.join(2)
        if self.log: self.log.close()
        if self.lease:
            self.lease.release()
            self.lease = None
    def __exit__(self, *args): self.close()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    parser.add_argument('--keep-logs', action='store_true', help='Preserve temporary log directory after exit')
    args = parser.parse_args()
    config = Config.load(args.config)
    work = Path(tempfile.mkdtemp(prefix='zci-chat-'))
    work.chmod(0o700)
    gateway = None
    def stop(signum, frame): raise KeyboardInterrupt
    # Cover model startup too, before the child can be spawned.
    signal.signal(signal.SIGTERM, stop)
    try:
        with Engine(config, work):
            gateway = Gateway(config).start()
            print(f'ZCI sohbet: http://127.0.0.1:{config.gateway_port}/v1 ; kayıt: {work}', flush=True)
            try:
                while True: time.sleep(1)
            finally: gateway.close()
    except KeyboardInterrupt:
        pass
    finally:
        if gateway: gateway.backend.close()
        if not args.keep_logs: shutil.rmtree(work, ignore_errors=True)

if __name__ == '__main__': main()

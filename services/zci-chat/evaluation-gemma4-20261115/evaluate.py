"""One-shot, CPU-only evaluation; never accesses WebUI or existing services."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT.parent
BASE = Path('/home/Zevor/.local/share/zci')
MODEL = BASE / 'models/evaluation/gemma4-e4b-q4-k-m/gemma-4-E4B-it-Q4_K_M.gguf'
ENGINE = BASE / 'engine/llama.cpp/build-cpu/bin/llama-server'
PORT = 28089
ALIAS = 'zci-model-evaluation'
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def save(name, value):
    (ROOT / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def available():
    return int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines()
                    if line.startswith('MemAvailable:'))) * 1024


def stats(pid):
    status = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in line)
    return {key: int(status.get(key, '0 kB').split()[0]) for key in ('VmRSS', 'VmHWM')} | {'available_bytes': available()}


def api(path, body=None):
    data = None if body is None else json.dumps(body, ensure_ascii=False).encode()
    request = urllib.request.Request(f'http://127.0.0.1:{PORT}{path}', data=data,
                                     headers={'Content-Type': 'application/json'})
    with OPENER.open(request, timeout=15) as response:
        return json.load(response)


def protected():
    # Models are checked by inode/size/mtime, without reading or running ZEKANS.
    paths = [BASE / 'models/Qwen3-1.7B-Q4_K_M.gguf',
             BASE / 'models/evaluation/qwen3-4b-q4-k-m/Qwen3-4B-Q4_K_M.gguf', ENGINE]
    result = {}
    for path in paths:
        st = path.stat()
        result[str(path)] = {'inode': st.st_ino, 'size': st.st_size, 'mtime_ns': st.st_mtime_ns}
    for path in [SOURCE / 'system.md', SOURCE / 'config.example.json',
                 Path('/home/Zevor/Zethropol-OS/AGENTS.md')]:
        result[str(path)] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
    return result


class Server:
    def __enter__(self):
        assert available() > 12 * 1024**3, 'Insufficient starting RAM'
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(('127.0.0.1', PORT))
        self.samples = []
        self.abort = None
        self.stop = threading.Event()
        self.log = (ROOT / 'server.log').open('x')
        self.command = [str(ENGINE), '--model', str(MODEL), '--alias', ALIAS,
                        '--host', '127.0.0.1', '--port', str(PORT), '--device', 'none',
                        '--n-gpu-layers', '0', '--no-op-offload', '--threads', '6',
                        '--threads-batch', '6', '--threads-http', '2', '--ctx-size', '32768',
                        '--parallel', '1', '--batch-size', '512', '--ubatch-size', '128',
                        '--offline', '--no-webui', '--no-agent', '--no-ui-mcp-proxy',
                        '--cors-origins', 'localhost', '--jinja', '--reasoning', 'off', '--no-context-shift']
        save('command.json', self.command)
        began = time.monotonic()
        self.process = subprocess.Popen(self.command, stdin=subprocess.DEVNULL,
                                        stdout=self.log, stderr=subprocess.STDOUT,
                                        env={'HOME': '/home/Zevor', 'PATH': '/usr/bin:/bin',
                                             'LANG': 'C.UTF-8', 'OMP_NUM_THREADS': '6'})
        def monitor():
            while not self.stop.wait(.2):
                if self.process.poll() is not None:
                    return
                try:
                    sample = stats(self.process.pid)
                except OSError:
                    continue
                self.samples.append(sample)
                if sample['available_bytes'] < 6 * 1024**3 or sample['VmRSS'] > 18 * 1024**2:
                    self.abort = 'RAM safety threshold reached'
                    self.process.terminate()
                    return
        self.thread = threading.Thread(target=monitor, daemon=True)
        self.thread.start()
        try:
            while time.monotonic() - began < 180:
                if self.process.poll() is not None:
                    raise RuntimeError('Model load failed; inspect server.log')
                try:
                    if api('/health').get('status') == 'ok':
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(.3)
            else:
                raise TimeoutError('Model load timeout')
            props = api('/props')
            assert props['default_generation_settings']['n_ctx'] == 32768
            save('props.json', props)
            template = props['chat_template']
            messages = [{'role': 'system', 'content': 'Türkçe yanıt ver.'},
                        {'role': 'user', 'content': 'Merhaba.'},
                        {'role': 'assistant', 'content': 'Merhaba.'},
                        {'role': 'user', 'content': 'Önceki yanıtı hatırla.'}]
            rendered = api('/apply-template', {'messages': messages,
                                               'chat_template_kwargs': {'enable_thinking': False}})['prompt']
            if MODEL.name.startswith('gemma-4'):
                assert '<|turn>' in template and '<turn|>' in template
                assert rendered.count('<|turn>system') == 1
                assert '<|turn>user' in rendered and '<|turn>model' in rendered
                assert '<|think|>' not in rendered, 'Thinking unexpectedly enabled'
            else:
                assert rendered.count('<|im_start|>system') == 1
                assert '<think>\n\n</think>' in rendered, 'Non-thinking Qwen template not confirmed'
            save('template-check.json', {'template_sha256': hashlib.sha256(template.encode()).hexdigest(),
                                         'rendered': rendered, 'embedded_template': True,
                                         'thinking': False, 'load_seconds': time.monotonic() - began})
            print('READY', stats(self.process.pid), flush=True)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *args):
        if self.process.poll() is None:
            self.samples.append(stats(self.process.pid))
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.stop.set()
        self.thread.join(timeout=2)
        self.log.close()
        save('lifecycle.json', {'pid': self.process.pid, 'stopped': self.process.poll() is not None,
                                'returncode': self.process.returncode, 'resource_abort': self.abort,
                                'peak_rss_kib': max((s['VmRSS'] for s in self.samples), default=0),
                                'peak_hwm_kib': max((s['VmHWM'] for s in self.samples), default=0),
                                'minimum_available_bytes': min((s['available_bytes'] for s in self.samples), default=0)})

    def infer(self, messages, limit=768):
        prompt = api('/apply-template', {'messages': messages,
                                         'chat_template_kwargs': {'enable_thinking': False}})['prompt']
        tokens = len(api('/tokenize', {'content': prompt, 'add_special': True, 'parse_special': True})['tokens'])
        assert tokens + limit <= 32768
        payload = {'model': ALIAS, 'messages': messages, 'max_tokens': limit,
                   'temperature': .2, 'seed': 7, 'cache_prompt': False,
                   'top_k': 64, 'top_p': .95, 'min_p': .05,
                   'chat_template_kwargs': {'enable_thinking': False},
                   'stream': True, 'stream_options': {'include_usage': True}}
        began = time.monotonic()
        first = last = None
        answer = ''
        usage = {}
        timings = {}
        finish = None
        request = urllib.request.Request(f'http://127.0.0.1:{PORT}/v1/chat/completions',
                                         data=json.dumps(payload, ensure_ascii=False).encode(),
                                         headers={'Content-Type': 'application/json'})
        with OPENER.open(request, timeout=180) as response:
            for line in response:
                if time.monotonic() - began > 180:
                    raise TimeoutError('Response exceeded 180 seconds')
                if not line.startswith(b'data: '):
                    continue
                raw = line[6:].strip()
                if raw == b'[DONE]':
                    break
                chunk = json.loads(raw)
                if chunk.get('error'):
                    raise RuntimeError(str(chunk['error']))
                usage = chunk.get('usage') or usage
                timings = chunk.get('timings') or timings
                for choice in chunk.get('choices', []):
                    text = choice.get('delta', {}).get('content') or ''
                    if text:
                        now = time.monotonic()
                        first = now if first is None else first
                        last = now
                        answer += text
                    finish = choice.get('finish_reason') or finish
        elapsed = time.monotonic() - began
        assert usage.get('prompt_tokens') == tokens, (tokens, usage)
        return {'answer': answer, 'messages': messages, 'input_tokens': tokens, 'usage': usage,
                'finish_reason': finish, 'total_seconds': elapsed,
                'first_content_seconds': None if first is None else first - began,
                'observed_generation_tokens_per_second': (usage['completion_tokens'] - 1) / (last - first)
                if first is not None and last > first else None, 'server_timings': timings,
                'stats_after': stats(self.process.pid)}


def main():
    global ROOT, MODEL
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus', type=Path)
    parser.add_argument('--compatibility-only', action='store_true')
    parser.add_argument('--reference-qwen', action='store_true', help='Evaluate only the existing Qwen3-4B file')
    args = parser.parse_args()
    if args.reference_qwen:
        MODEL = BASE / 'models/evaluation/qwen3-4b-q4-k-m/Qwen3-4B-Q4_K_M.gguf'
        ROOT = ROOT / 'qwen3-4b'
        ROOT.mkdir(exist_ok=True)
    assert args.compatibility_only or args.corpus, 'Explicit corpus required'
    assert not (ROOT / 'lifecycle.json').exists(), 'One-shot result already exists; refusing to overwrite'
    corpus = None
    if args.corpus:
        document = json.loads(args.corpus.read_text())
        assert document['schema'] == 'zci-turkish-conversation-acceptance-v1'
        corpus = document['cases']
        assert sum(len(c['turns']) for c in corpus) == 9
        save('acceptance-frozen.json', document)
        save('corpus-provenance.json', {'source': str(args.corpus.resolve()),
                                       'sha256': hashlib.sha256(args.corpus.read_bytes()).hexdigest(),
                                       'version': document['version'], 'frozen_before_inference': True})
    if args.reference_qwen:
        expected = '7485fe6f11af29433bc51cab58009521f205840f5b4ae3a32fa7f92e8534fdf5'
        with MODEL.open('rb') as source:
            actual = hashlib.file_digest(source, 'sha256').hexdigest()
        assert actual == expected
        result = {'path': str(MODEL), 'sha256': actual, 'verified': True, 'new_download': False}
    else:
        result = json.loads((MODEL.parent / 'download-result.json').read_text())
        assert result['verified'] and result['sha256'] == '0ffb122c8b6921f13cbc34186e052524d0b5803b17f4867b7197a561400b3770'
        assert MODEL.stat().st_size == 5335291936
    save('download-result.json', result)
    before = protected()
    save('protected-before.json', before)
    rows = []
    try:
        with Server() as server:
            if corpus is not None:
                system = (SOURCE / 'system.md').read_text()
                save('settings.json', {'temperature': .2, 'seed': 7, 'output': 768, 'cache_prompt': False,
                                        'top_k': 64, 'top_p': .95, 'min_p': .05,
                                        'context': 32768, 'threads': 6, 'thinking': False,
                                        'system_sha256': hashlib.sha256(system.encode()).hexdigest()})
                for case in corpus:
                    history = [{'role': 'system', 'content': system}, *case.get('prelude', [])]
                    for turn, item in enumerate(case['turns'], 1):
                        history.append({'role': 'user', 'content': item['user']})
                        row = {'case': case['id'], 'turn': turn, 'id': item['id'], 'expected': item.get('expected'),
                               **server.infer(list(history))}
                        if 'exact' in item:
                            row['exact_pass'] = row['answer'].strip() == item['exact']
                        rows.append(row)
                        save('responses.json', rows)
                        history.append({'role': 'assistant', 'content': row['answer']})
                        print(json.dumps(row, ensure_ascii=False), flush=True)
                save('metrics.json', {'turns': len(rows),
                                       'median_response_seconds': statistics.median(r['total_seconds'] for r in rows),
                                       'median_first_content_seconds': statistics.median(r['first_content_seconds'] for r in rows if r['first_content_seconds'] is not None),
                                       'median_generation_tokens_per_second': statistics.median(r['observed_generation_tokens_per_second'] for r in rows if r['observed_generation_tokens_per_second'] is not None)})
    finally:
        after = protected()
        save('protected-after.json', {'files': after, 'unchanged': before == after})


if __name__ == '__main__':
    main()

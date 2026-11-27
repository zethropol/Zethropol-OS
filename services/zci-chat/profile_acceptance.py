"""Real Gemma gateway acceptance using the frozen corpus and owned CPU process.

Invoke under the shared evaluation flock. Records synthetic test traffic only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import statistics
import sys
import tempfile
import threading
import time
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import zci_chat as z

BASE = Path(__file__).resolve().parent
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n')


def stats(pid):
    data = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in line)
    return {key: int(data.get(key, '0 kB').split()[0]) for key in ('VmRSS', 'VmHWM')} | {'available_bytes': z.available_ram()}


def ports():
    result = {}
    for port in (8087, 8088):
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=1): result[str(port)] = 'listening'
        except ConnectionRefusedError: result[str(port)] = 'closed'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=('focus', 'full'), required=True)
    parser.add_argument('--config', type=Path, default=BASE/'config.gemma4.example.json')
    args = parser.parse_args()
    config = z.Config.load(args.config)
    assert config.profile == 'gemma4-e4b'
    corpus_path = BASE/'tests/acceptance/tr-v1/acceptance.json'
    document = json.loads(corpus_path.read_text())
    turns = document['cases'][0]['turns']
    assert len(turns) == 9
    if args.stage == 'focus': turns = turns[:2]
    work = Path(tempfile.mkdtemp(prefix='zci-gemma-'+args.stage+'-'))
    print('ARTIFACTS', work, flush=True)
    protected = {str(path): z.file_hash(path) for path in (BASE/'system.md', BASE/'config.example.json', BASE.parents[1]/'AGENTS.md')}
    report = {'stage': args.stage, 'profile': config.profile,
              'corpus_sha256': z.file_hash(corpus_path), 'corpus_version': document['version'],
              'system_sha256': z.file_hash(config.system_file), 'existing_ports_before': ports(),
              'profile_parameters': {'temperature': .2, 'top_k': 64, 'top_p': .95, 'min_p': .05, 'seed': 7,
                                     'output': 768, 'context': 32768, 'thinking': False},
              'turns': [], 'cleanup': {}}
    stop = threading.Event(); samples = []; gateway = None
    history = []
    try:
        with z.Engine(config, work) as engine:
            def monitor():
                while not stop.wait(.2):
                    try: samples.append(stats(engine.process.pid))
                    except OSError: return
            sampler = threading.Thread(target=monitor, daemon=True); sampler.start()
            gateway = z.Gateway(config).start()
            original_prepare = gateway.backend.prepare; prepared = []
            def prepare(body):
                result = original_prepare(body); prepared.append(result[0]); return result
            gateway.backend.prepare = prepare
            report['template_sha256'] = gateway.backend.identity()
            props = gateway.backend.api('/props')
            save(work/'props.json', props)
            assert props['default_generation_settings']['n_ctx'] == 32768
            for number, item in enumerate(turns, 1):
                history.append({'role': 'user', 'content': item['user']})
                body = {'model': config.alias, 'messages': list(history), 'stream': True, 'max_tokens': 768}
                request = urllib.request.Request(f'http://127.0.0.1:{config.gateway_port}/v1/chat/completions',
                    data=json.dumps(body, ensure_ascii=False).encode(), headers={'Content-Type': 'application/json'})
                began = time.monotonic(); first = None; answer = ''; usage = {}; finish = status = None
                with OPENER.open(request, timeout=180) as response:
                    for line in response:
                        if not line.startswith(b'data: '): continue
                        if line[6:].strip() == b'[DONE]': break
                        packet = json.loads(line[6:]); usage = packet.get('usage') or usage
                        status = packet.get('zci_status') or status
                        for choice in packet.get('choices', []):
                            text = choice.get('delta', {}).get('content') or ''
                            if text and first is None: first = time.monotonic()-began
                            answer += text; finish = choice.get('finish_reason') or finish
                elapsed = time.monotonic()-began
                sent = prepared[-1]
                assert sent['messages'] == [{'role': 'system', 'content': gateway.backend.system}]+history
                assert sent['chat_template_kwargs'] == {'enable_thinking': False}
                assert sent['top_k'] == 64 and sent['min_p'] == .05
                assert finish == 'stop' and status == 'completed' and usage and answer.strip()
                assert not any(marker in answer for marker in z.TAGS)
                assert usage['prompt_tokens']+768 <= 32768
                generation = None
                for line in (work/'engine.log').read_text().splitlines():
                    if 'eval time =' in line and 'prompt eval time' not in line:
                        found = re.search(r'([0-9.]+) tokens per second', line)
                        if found: generation = float(found.group(1))
                row = {'turn': number, 'id': item['id'], 'user': item['user'], 'expected': item['expected'],
                       'answer': answer, 'seconds': elapsed, 'first_content_seconds': first,
                       'server_generation_tokens_per_second': generation, 'usage': usage,
                       'finish_reason': finish, 'zci_status': status, 'history_exact': True,
                       'model': config.alias, 'stats_after': stats(engine.process.pid)}
                if 'exact' in item: row['exact_pass'] = answer.strip() == item['exact']
                report['turns'].append(row); save(work/'result.json', report)
                history.append({'role': 'assistant', 'content': answer})
                print(json.dumps({k: row[k] for k in ('turn', 'id', 'answer', 'seconds', 'first_content_seconds')}, ensure_ascii=False), flush=True)
            samples.append(stats(engine.process.pid)); stop.set(); sampler.join(2)
            gateway.close(); gateway = None
        report['metrics'] = {'median_response_seconds': statistics.median(r['seconds'] for r in report['turns']),
            'median_first_content_seconds': statistics.median(r['first_content_seconds'] for r in report['turns']),
            'median_server_generation_tokens_per_second': statistics.median(r['server_generation_tokens_per_second'] for r in report['turns']),
            'peak_rss_kib': max(s['VmRSS'] for s in samples), 'peak_hwm_kib': max(s['VmHWM'] for s in samples),
            'minimum_available_bytes': min(s['available_bytes'] for s in samples)}
    finally:
        stop.set()
        if gateway: gateway.close()
        report['cleanup'] = {'engine_stopped': 'engine' not in locals() or engine.process is None or engine.process.poll() is not None,
                             'gateway_stopped': gateway is None or gateway.thread is None or not gateway.thread.is_alive()}
        report['existing_ports_after'] = ports()
        report['protected_sources_unchanged'] = protected == {str(path): z.file_hash(path) for path in map(Path, protected)}
        report['existing_ports_unchanged'] = report['existing_ports_before'] == report['existing_ports_after']
        save(work/'result.json', report)
        print('RESULT', work/'result.json', flush=True)


if __name__ == '__main__': main()

"""Opt-in five-turn conversation and exact-history comparison in a disposable UI.

Captures only synthetic requests sent through this test's private gateway.
Never records traffic from the real WebUI or changes real accounts/chats.
"""
import asyncio
import argparse
import copy
from dataclasses import replace
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parent))
import zci_chat as z
from live_acceptance import Browser, WEBUI, http, wait_http, stop

QUESTIONS = [
    'doğru sohbet başlatma nasıl olur',
    'bir sohbet nasıl başlatılmalıdır',
    'Yeni tanıştığım iş arkadaşımla konuşacağım. Bana yalnız bir açılış cümlesi öner.',
    'Az önce kiminle konuşacağımı söylemiştim? Tek kısa cümleyle yanıtla.',
    'Sedef-928 adlı hayalî protokolün başlık uzunluğunu belirtmedim. Kaç bayt olduğunu biliyor musun?',
]

async def dialogue(work, token, gateway, traces, variants):
    browser = Browser(work, debug_port=28092)
    rows = []
    try:
        await browser.start()
        await browser.navigate('http://127.0.0.1:28088/auth')
        await browser.evaluate('localStorage.setItem("token", '+json.dumps(token)+'); true')
        for variant in variants:
            system = (work/('system.before.md' if variant == 'before' else 'candidate.md')).read_text().strip()
            gateway.backend.system = system  # Only this owned test instance, between finished turns.
            await browser.navigate('http://127.0.0.1:28088/?model='+z.ALIAS)
            expected = []
            previous_bubble = None
            for turn, question in enumerate(QUESTIONS, 1):
                expected.append({'role': 'user', 'content': question})
                index = len(traces); began = time.monotonic()
                await browser.send(question)
                deadline = time.monotonic()+150
                answer = ''
                while time.monotonic() < deadline:
                    if len(traces) > index and traces[index].get('done'):
                        value = await browser.evaluate("JSON.stringify(Array.from(document.querySelectorAll('#response-content-container')).map(e=>({id:e.closest('[id^=\"message-\"]')?.id,text:e.innerText})))")
                        answers = json.loads(value or '[]')
                        # Virtualization can remove old bubbles; Markdown rendering
                        # also differs from raw SSE text. Require a new bubble ID.
                        if answers and answers[-1]['id'] != previous_bubble and answers[-1]['text'].strip():
                            answer = answers[-1]['text'].strip()
                            previous_bubble = answers[-1]['id']; break
                    await asyncio.sleep(.3)
                if not answer:
                    (work/'timeout-ui.html').write_text(await browser.evaluate('document.body.innerHTML'))
                    raise RuntimeError('Conversation turn did not complete: '+variant+'/'+str(turn))
                trace = traces[index]
                incoming = [m for m in trace['incoming']['messages'] if m['role'] != 'system']
                outgoing = [m for m in trace['prepared']['messages'] if m['role'] != 'system']
                # Browser rendering is not used to rewrite the next request. The actual
                # frontend must send its own stored assistant messages unchanged.
                assert incoming == expected, ('frontend history mismatch', variant, turn)
                assert outgoing == incoming, ('gateway history mismatch', variant, turn)
                assert len([m for m in trace['prepared']['messages'] if m['role'] == 'system']) == 1
                row = {'variant': variant, 'turn': turn, 'user': question, 'answer': answer,
                       'seconds': round(time.monotonic()-began, 3),
                       'user_assistant_messages': len(incoming), 'history_exact': True,
                       'system_prompt_count': 1, 'prompt_tokens': trace['prompt_tokens'],
                       'finish_reason': trace.get('finish_reason'), 'stream': trace['prepared']['stream']}
                rows.append(row)
                (work/'dialogues.json').write_text(json.dumps(rows, ensure_ascii=False, indent=2)+'\n')
                (work/'synthetic-history-traces.json').write_text(json.dumps(traces, ensure_ascii=False, indent=2)+'\n')
                print(json.dumps(row, ensure_ascii=False), flush=True)
                # Compare the next native WebUI request against raw SSE content,
                # rather than the Markdown-rendered bubble text.
                expected.append({'role': 'assistant', 'content': trace['final_text']})
            await browser.screenshot('dialogue-'+variant+'.png')
        return rows
    finally:
        await browser.close()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--candidate-only', action='store_true')
    options = parser.parse_args()
    if not options.run: raise SystemExit('Explicit --run required')
    work = Path(tempfile.mkdtemp(prefix='zci-conversation-'))
    base = Path(__file__).resolve().parent
    (work/'system.before.md').write_text((base/'system.md').read_text())
    print('ARTIFACTS', work, flush=True)
    (work/'home').mkdir(exist_ok=True); (work/'webui-data').mkdir(mode=0o700, exist_ok=True)
    original = (work/'system.before.md').read_text()
    candidate = original.replace(
        'Asistan adı: ZCI (Zethropol Core Intelligence). Bu bilgisayarda yerel olarak çalışan bir sohbet modelidir.',
        'Kimliğini yalnız sorulduğunda belirt: ZCI. Diğer sorularda kendini tanıtma.').replace(
        'Her yanıta doğrudan son sorunun cevabıyla başla.',
        'Son soruyu önceki kullanıcı ve asistan mesajlarıyla birlikte değerlendirerek doğrudan yanıtla.').replace(
        'Kimlik tanıtımı yalnızca kim olduğun sorulduğunda, selamlama yalnızca konuşmanın başlangıcında uygundur.',
        'Kullanıcı selam vermedikçe ona selamla hitap etme; istenen örnek cümleler bu kuralın dışındadır.')
    candidate = candidate.replace('Soruyu somut bilgi, açıklama veya örnekle karşıla.',
        'Soruyu somut bilgi, açıklama veya örnekle karşıla. Konuşmada verilmeyen veya bilmediğin bir bilgiyi tahmin ederek gerçekmiş gibi sunma; bilmediğini belirt.')
    assert candidate != original
    (work/'candidate.md').write_text(candidate)
    config = z.Config.load(base/'config.example.json')
    external = False
    try:
        z.Backend(config).identity()
        external = True
    except z.ChatError:
        config = replace(config, engine_port=28087)
    config = replace(config, gateway_port=28086)
    for port in [28086, 28088, 28092]+([] if external else [28087]):
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(('127.0.0.1', port))
    engine = None; gateway = None; ui = None; log = None; traces = []
    report = {'existing_verified_4b_engine_reused': external, 'owns_engine': not external}
    try:
        if external:
            assert z.file_hash(config.model) == config.model_sha256
        else:
            engine = z.Engine(config, work); engine.__enter__()
        gateway = z.Gateway(config).start()
        original_prepare = gateway.backend.prepare
        original_stream = z.Handler.stream
        def prepare(body):
            payload, count, identity, model = original_prepare(body)
            traces.append({'incoming': copy.deepcopy(body), 'prepared': copy.deepcopy(payload),
                           'prompt_tokens': count, 'done': False})
            return payload, count, identity, model
        gateway.backend.prepare = prepare
        # Capture only final content from the owned test gateway. SSE chunks pass
        # through the real codec; use an in-process tee on its response writer.
        def stream(handler, *args):
            trace = traces[-1]; writer = handler.wfile; packets = []
            class Tee:
                def write(self, value):
                    packets.append(value); return writer.write(value)
                def flush(self): return writer.flush()
                def __getattr__(self, name): return getattr(writer, name)
            handler.wfile = Tee()
            try: return original_stream(handler, *args)
            finally:
                handler.wfile = writer
                text = ''; finish = None
                for line in b''.join(packets).splitlines():
                    if not line.startswith(b'data: ') or line[6:].strip() == b'[DONE]': continue
                    packet = json.loads(line[6:])
                    for choice in packet.get('choices', []):
                        text += choice.get('delta', {}).get('content') or ''
                        finish = choice.get('finish_reason') or finish
                trace.update(final_text=text, finish_reason=finish, done=True)
                (work/'synthetic-history-traces.json').write_text(json.dumps(traces, ensure_ascii=False, indent=2)+'\n')
        z.Handler.stream = stream
        env = {'HOME': str(work/'home'), 'PATH': str(WEBUI/'venv/bin')+':/usr/bin:/bin',
            'LANG': 'C.UTF-8', 'ENV': 'prod', 'PYTHONDONTWRITEBYTECODE': '1', 'PYTHON_DOTENV_DISABLED': '1',
            'DATA_DIR': str(work/'webui-data'), 'DATABASE_URL': 'sqlite:///'+str(work/'webui-data/webui.db'),
            'FRONTEND_BUILD_DIR': str(WEBUI/'venv/lib/python3.12/site-packages/open_webui/frontend'),
            'STATIC_DIR': str(work/'webui-static'), 'LICENSE_BLOB_PATH': str(work/'test-license'),
            'WEBUI_AUTH': 'False', 'WEBUI_SECRET_KEY': 'isolated-conversation-test-only',
            'ENABLE_PERSISTENT_CONFIG': 'False', 'ENABLE_OLLAMA_API': 'False', 'ENABLE_OPENAI_API': 'True',
            'OPENAI_API_BASE_URL': 'http://127.0.0.1:28086/v1', 'OPENAI_API_BASE_URLS': 'http://127.0.0.1:28086/v1',
            'OPENAI_API_KEYS': 'unused-local', 'DEFAULT_MODELS': z.ALIAS, 'DEFAULT_LOCALE': 'tr-TR',
            'DEFAULT_MODEL_PARAMS': json.dumps({'max_tokens': 384, 'temperature': .2, 'seed': 7}),
            'DEFAULT_MODEL_METADATA': json.dumps({'capabilities': {'builtin_tools': False, 'vision': False, 'code_interpreter': False, 'web_search': False}}),
            'OFFLINE_MODE': 'True', 'HF_HUB_OFFLINE': '1', 'USE_SLIM_DOCKER': 'True',
            'DO_NOT_TRACK': 'true', 'ENABLE_VERSION_UPDATE_CHECK': 'False',
            'ENABLE_CODE_EXECUTION': 'False', 'ENABLE_CODE_INTERPRETER': 'False',
            'ENABLE_WEB_SEARCH': 'False', 'ENABLE_IMAGE_GENERATION': 'False',
            'ENABLE_AUTOCOMPLETE_GENERATION': 'False', 'ENABLE_FOLLOW_UP_GENERATION': 'False',
            'ENABLE_TAGS_GENERATION': 'False', 'ENABLE_TITLE_GENERATION': 'False',
            'ENABLE_CONTEXT_COMPACTION': 'False', 'ENABLE_MEMORIES': 'False',
            'ENABLE_MEMORY_SYSTEM_CONTEXT': 'False', 'ENABLE_MEMORY_BACKGROUND_REVIEW': 'False',
            'RAG_EMBEDDING_ENGINE': 'openai', 'NO_PROXY': 'localhost,127.0.0.1'}
        log = (work/'webui.log').open('w')
        ui = subprocess.Popen([str(WEBUI/'venv/bin/python'), '-B', '-m', 'uvicorn', 'open_webui.main:app',
            '--host', '127.0.0.1', '--port', '28088', '--workers', '1'], cwd=work, env=env,
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        wait_http('http://127.0.0.1:28088', '/health', ui)
        auth = http('http://127.0.0.1:28088', '/api/v1/auths/signin', {'email': 'test@example.invalid', 'password': 'synthetic'})
        variants = ['candidate'] if options.candidate_only else ['before', 'candidate']
        rows = asyncio.run(dialogue(work, auth['token'], gateway, traces, variants))
        report['turns'] = len(rows); report['all_histories_exact'] = all(r['history_exact'] for r in rows)
    except Exception as error:
        report['error_type'] = type(error).__name__
        raise
    finally:
        stop(ui)
        if gateway: gateway.close()
        if engine: engine.close()
        z.Handler.stream = original_stream if 'original_stream' in locals() else z.Handler.stream
        if log: log.close()
        report['completed_gateway_turns'] = sum(t.get('done', False) for t in traces)
        report['cleanup'] = {'webui_stopped': ui is None or ui.poll() is not None,
                             'owned_engine_stopped': engine is None or engine.process is None or engine.process.poll() is not None,
                             'active_upstreams': len(gateway.backend.active) if gateway else 0}
        (work/'result.json').write_text(json.dumps(report, indent=2)+'\n')
        print('RESULT', work/'result.json', flush=True)

if __name__ == '__main__': main()

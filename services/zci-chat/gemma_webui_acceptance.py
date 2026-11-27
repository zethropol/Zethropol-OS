"""Open WebUI 0.13 Gemma selection in a fresh DB with a preserved fixture default.

No production settings, accounts, tokens, browser profiles or chats are opened.
"""
import asyncio
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import zci_chat as z
from live_acceptance import Browser, WEBUI, http, wait_http, stop

BASE = Path(__file__).resolve().parent
DEFAULT = 'zci-test-preserved-default'
UI = 'http://127.0.0.1:19688'


class DefaultFixture(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        raw = json.dumps({'object': 'list', 'data': [{'id': DEFAULT, 'object': 'model', 'owned_by': 'synthetic-default-fixture'}]}).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json'); self.end_headers(); self.wfile.write(raw)
    def do_POST(self):
        self.server.inference_attempts += 1
        self.send_response(503); self.send_header('Content-Type', 'application/json'); self.end_headers()
        self.wfile.write(b'{"detail":"Synthetic default is intentionally not an inference model"}')


async def browser_check(work, token, config, engine, gateway, routed):
    browser = Browser(work, debug_port=19692)
    try:
        await browser.start(); await browser.navigate(UI+'/auth')
        await browser.evaluate('localStorage.setItem("token", '+json.dumps(token)+'); true')
        await browser.navigate(UI+'/?model='+config.alias)
        before = len(routed)
        await browser.send('Yalnız TAMAM yaz.')
        deadline = time.monotonic()+90; answer = ''
        while time.monotonic() < deadline:
            if len(routed) > before and not gateway.backend.turn.locked():
                answer = await browser.evaluate("Array.from(document.querySelectorAll('#response-content-container')).map(e=>e.innerText).join('\\n')") or ''
                if 'tamam' in answer.casefold(): break
            await asyncio.sleep(.3)
        assert answer and 'tamam' in answer.casefold(), 'Gemma response not rendered by actual UI'
        assert routed[-1]['model'] == config.alias and routed[-1]['stream'] is True
        assert not any(marker in answer for marker in z.TAGS)
        await browser.screenshot('gemma-selected.png')
        (work/'browser-answer.txt').write_text(answer)
        # Only our owned engine is stopped. The UI must display an error rather than fallback.
        engine.close()
        await browser.send('Kısa bir yanıt ver.')
        deadline = time.monotonic()+45; error = ''
        while time.monotonic() < deadline:
            error = await browser.evaluate('document.body.innerText') or ''
            if any(text in error for text in ('bağlantı kurulamadı', 'modeliyle', '503', 'Service Unavailable')): break
            await asyncio.sleep(.5)
        else: raise RuntimeError('Unavailable Gemma error not visible')
        await browser.screenshot('gemma-unavailable.png')
        return {'actual_firefox_ui': True, 'selected_model': config.alias, 'sse_bubble': True,
                'reasoning_control_tokens_absent': True, 'owned_engine_unavailable_error_visible': True}
    finally:
        await browser.close()


def main():
    if '--run' not in sys.argv: raise SystemExit('Explicit --run required')
    config = z.Config.load(BASE/'config.gemma4.example.json')
    work = Path(tempfile.mkdtemp(prefix='zci-gemma-webui-'))
    print('ARTIFACTS', work, flush=True)
    (work/'home').mkdir(); (work/'webui-data').mkdir(mode=0o700)
    for port in (config.engine_port, config.gateway_port, 19685, 19688, 19692):
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); probe.bind(('127.0.0.1', port))
    fixture = ThreadingHTTPServer(('127.0.0.1', 19685), DefaultFixture)
    fixture.daemon_threads = True; fixture.inference_attempts = 0
    fixture_thread = threading.Thread(target=fixture.serve_forever, daemon=True); fixture_thread.start()
    gateway = ui = log = None; routed = []
    report = {'real_user_database_opened': False, 'default_fixture': DEFAULT, 'checks': {}, 'cleanup': {}}
    try:
        with z.Engine(config, work) as engine:
            gateway = z.Gateway(config).start(); original_prepare = gateway.backend.prepare
            def prepare(body):
                result = original_prepare(body)
                routed.append({'model': body['model'], 'stream': result[0]['stream'],
                               'thinking': result[0]['chat_template_kwargs']['enable_thinking']})
                return result
            gateway.backend.prepare = prepare
            provider_urls = 'http://127.0.0.1:19685/v1;http://127.0.0.1:'+str(config.gateway_port)+'/v1'
            env = {'HOME': str(work/'home'), 'PATH': str(WEBUI/'venv/bin')+':/usr/bin:/bin',
                'LANG': 'C.UTF-8', 'ENV': 'prod', 'PYTHONDONTWRITEBYTECODE': '1', 'PYTHON_DOTENV_DISABLED': '1',
                'DATA_DIR': str(work/'webui-data'), 'DATABASE_URL': 'sqlite:///'+str(work/'webui-data/webui.db'),
                'FRONTEND_BUILD_DIR': str(WEBUI/'venv/lib/python3.12/site-packages/open_webui/frontend'),
                'STATIC_DIR': str(work/'webui-static'), 'LICENSE_BLOB_PATH': str(work/'test-license'),
                'WEBUI_AUTH': 'False', 'WEBUI_SECRET_KEY': 'isolated-gemma-test-only',
                'ENABLE_PERSISTENT_CONFIG': 'False', 'ENABLE_OLLAMA_API': 'False', 'ENABLE_OPENAI_API': 'True',
                'OPENAI_API_BASE_URL': 'http://127.0.0.1:19685/v1', 'OPENAI_API_BASE_URLS': provider_urls,
                'OPENAI_API_KEYS': 'synthetic;synthetic', 'DEFAULT_MODELS': DEFAULT, 'DEFAULT_LOCALE': 'tr-TR',
                'DEFAULT_MODEL_PARAMS': json.dumps({'max_tokens': 32}),
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
                '--host', '127.0.0.1', '--port', '19688', '--workers', '1'], cwd=work,
                env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            wait_http(UI, '/health', ui)
            version = http(UI, '/api/version').get('version', '')
            assert version in ('0.13.0', '0.0.0', 'v0.0.0') or version.startswith('0.')
            auth = http(UI, '/api/v1/auths/signin', {'email': 'test@example.invalid', 'password': 'synthetic'})
            token = auth['token']
            initial_default = http(UI, '/api/config', token=token)['default_models']
            assert initial_default == DEFAULT
            ids = [m['id'] for m in http(UI, '/api/models', token=token)['data']]
            assert DEFAULT in ids and config.alias in ids
            assert z.ALIAS not in ids and config.alias+'-thinking' not in ids
            result = http(UI, '/api/chat/completions', {'model': config.alias,
                'messages': [{'role': 'user', 'content': 'Yalnız TAMAM yaz.'}], 'stream': False,
                'max_tokens': 32}, token)
            assert 'tamam' in result['choices'][0]['message']['content'].casefold()
            assert routed[-1] == {'model': config.alias, 'stream': False, 'thinking': False}
            report['checks'] = {'webui_version': version, 'available_models': ids,
                'json_routed_to_gemma': True, 'default_before': initial_default}
            report['browser'] = asyncio.run(browser_check(work, token, config, engine, gateway, routed))
            final_default = http(UI, '/api/config', token=token)['default_models']
            assert final_default == initial_default
            assert fixture.inference_attempts == 0
            report['checks'].update(default_after=final_default, default_unchanged=True,
                default_fixture_inference_attempts=0, routing=routed)
            del auth, token
    finally:
        stop(ui)
        if gateway: gateway.close()
        if log: log.close()
        fixture.shutdown(); fixture.server_close(); fixture_thread.join(2)
        report['cleanup'] = {'ui_stopped': ui is None or ui.poll() is not None,
            'engine_stopped': 'engine' not in locals() or engine.process is None or engine.process.poll() is not None,
            'gateway_stopped': gateway is None or gateway.thread is None or not gateway.thread.is_alive(),
            'fixture_stopped': not fixture_thread.is_alive(),
            'active_upstreams': len(gateway.backend.active) if gateway else 0}
        (work/'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
        print('RESULT', work/'result.json', flush=True)


if __name__ == '__main__': main()

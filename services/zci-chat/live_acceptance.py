"""Opt-in real llama.cpp + isolated installed Open WebUI + Firefox acceptance.

Never loads the existing WebUI settings file, accounts, browser profile or chats.
Run with the existing WebUI Python to reuse its installed websockets dependency.
"""
import asyncio
import base64
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
sys.path.insert(0, str(Path(__file__).resolve().parent))
import zci_chat as z

WEBUI = Path.home() / '.local' / 'share' / 'zci' / 'webui'
OP = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def http(base, path, body=None, token=None, timeout=90):
    headers = {'Content-Type': 'application/json'}
    if token: headers['Authorization'] = 'Bearer '+token
    request = urllib.request.Request(base+path, data=json.dumps(body).encode() if body is not None else None,
                                     headers=headers)
    try:
        with OP.open(request, timeout=timeout) as r: return json.load(r)
    except urllib.error.HTTPError as error:
        status = error.code
        detail = error.read(2000).decode(errors='replace'); error.close()
        raise RuntimeError(f'Isolated API {path}: HTTP {status}: {detail}') from None

def wait_http(base, path, process, seconds=180):
    deadline = time.monotonic()+seconds
    while time.monotonic() < deadline:
        if process.poll() is not None: raise RuntimeError('Owned test process exited; inspect its private log.')
        try:
            return http(base, path, timeout=2)
        except (OSError, ValueError): time.sleep(.5)
    raise RuntimeError('Isolated process startup deadline')

def stop(process):
    if process and process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try: process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL); process.wait(timeout=5)

class Browser:
    def __init__(self, work, debug_port=18092):
        self.work = work; self.debug_port = debug_port
        self.process = None; self.ws = None; self.seq = 0
    async def start(self):
        import websockets
        profile = self.work/'firefox-profile'; profile.mkdir()
        prefs = {'browser.shell.checkDefaultBrowser': False, 'browser.aboutwelcome.enabled': False,
                 'browser.startup.homepage': 'about:blank', 'browser.newtabpage.enabled': False,
                 'toolkit.telemetry.enabled': False, 'datareporting.healthreport.uploadEnabled': False,
                 'network.captive-portal-service.enabled': False, 'network.connectivity-service.enabled': False,
                 'app.update.auto': False, 'app.normandy.enabled': False, 'extensions.update.enabled': False,
                 'browser.sessionstore.resume_from_crash': False}
        (profile/'user.js').write_text('\n'.join(f'user_pref({json.dumps(k)}, {json.dumps(v)});' for k, v in prefs.items()))
        self.log = (self.work/'firefox.log').open('w')
        self.process = subprocess.Popen(['/usr/bin/firefox', '--headless', '--no-remote', '--profile', str(profile),
            '--remote-debugging-port', str(self.debug_port), 'about:blank'], stdout=self.log, stderr=subprocess.STDOUT,
            cwd=self.work, start_new_session=True,
            env={'HOME': str(self.work/'home'), 'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8',
                 'XDG_CACHE_HOME': str(self.work/'browser-cache')})
        deadline = time.monotonic()+45
        while time.monotonic() < deadline:
            try:
                self.ws = await websockets.connect(f'ws://127.0.0.1:{self.debug_port}/session', max_size=8*1024*1024)
                break
            except OSError: await asyncio.sleep(.3)
        if not self.ws: raise RuntimeError('Isolated Firefox BiDi unavailable')
        await self.cmd('session.new', {'capabilities': {}})
        contexts = await self.cmd('browsingContext.getTree', {})
        self.context = contexts['contexts'][0]['context']
    async def cmd(self, method, params):
        self.seq += 1; ident = self.seq
        await self.ws.send(json.dumps({'id': ident, 'method': method, 'params': params}))
        while True:
            message = json.loads(await asyncio.wait_for(self.ws.recv(), 60))
            if message.get('id') != ident: continue
            if message.get('type') == 'error': raise RuntimeError(f'BiDi {method}: '+message.get('message', 'error'))
            return message.get('result', {})
    async def evaluate(self, expression):
        result = await self.cmd('script.evaluate', {'expression': expression, 'target': {'context': self.context},
                                                    'awaitPromise': True})
        if result.get('type') == 'exception': raise RuntimeError('Isolated browser script failed')
        return result.get('result', {}).get('value')
    async def navigate(self, url):
        await self.cmd('browsingContext.navigate', {'context': self.context, 'url': url, 'wait': 'complete'})
    async def wait_text(self, needle, seconds=90):
        deadline = time.monotonic()+seconds
        while time.monotonic() < deadline:
            text = await self.evaluate('document.body.innerText') or ''
            if needle in text: return text
            await asyncio.sleep(.5)
        (self.work/'ui-timeout.txt').write_text(text)
        raise RuntimeError('Expected text not rendered in isolated WebUI: '+needle)
    async def send(self, text):
        deadline = time.monotonic()+45
        while time.monotonic() < deadline:
            found = await self.evaluate("Boolean(document.querySelector('#chat-input'))")
            if found: break
            # Dismiss only this disposable UI's onboarding dialog if present.
            await self.evaluate("Array.from(document.querySelectorAll('button')).find(b=>/Get started|Başla|Okay|Tamam/.test(b.innerText))?.click()")
            await asyncio.sleep(.5)
        if not found: raise RuntimeError('Real WebUI chat input not found')
        await self.evaluate('''(() => {
            const e=document.querySelector('#chat-input'); e.focus();
            if(e.tagName==='TEXTAREA') { const setter=Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,'value').set; setter.call(e,TEXT); }
            else { e.innerText=TEXT; }
            e.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'insertText',data:TEXT}));
            return true;
        })()'''.replace('TEXT', json.dumps(text)))
        await asyncio.sleep(.4)  # Let the editor's DOM observer update its document.
        clicked = await self.evaluate("(() => {const b=document.querySelector('#send-message-button'); if(!b||b.disabled)return false;b.click();return true;})()")
        if not clicked: raise RuntimeError('Real WebUI send button not enabled')
    async def screenshot(self, name):
        result = await self.cmd('browsingContext.captureScreenshot', {'context': self.context})
        (self.work/name).write_bytes(base64.b64decode(result['data']))
    async def close(self):
        if self.ws: await self.ws.close()
        stop(self.process)
        if hasattr(self, 'log'): self.log.close()

async def browser_checks(work, token, engine, gateway):
    browser = Browser(work)
    try:
        await browser.start()
        await browser.navigate('http://127.0.0.1:18088/auth')
        # This token belongs only to the synthetic account in the new temporary DB.
        # It is never logged or written to the report.
        await browser.evaluate('localStorage.setItem("token", '+json.dumps(token)+'); true')
        await browser.navigate('http://127.0.0.1:18088/?model='+z.ALIAS)
        await browser.send('Yalnız TAMAM yaz.')
        deadline = time.monotonic()+60; normal = ''
        while time.monotonic() < deadline:
            normal = await browser.evaluate("Array.from(document.querySelectorAll('#response-content-container')).map(e=>e.innerText).join('\\n')")
            if normal and 'tamam' in normal.casefold(): break
            await asyncio.sleep(.5)
        while gateway.backend.turn.locked() and time.monotonic() < deadline:
            await asyncio.sleep(.2)
        assert not gateway.backend.turn.locked(), 'First turn did not finish'
        await browser.screenshot('ui-normal.png')
        (work/'ui-normal.txt').write_text(normal or '')
        if not normal or 'tamam' not in normal.casefold():
            (work/'ui-failure.html').write_text(await browser.evaluate('document.body.innerHTML'))
            raise RuntimeError('No actual assistant bubble rendered')
        await browser.navigate('http://127.0.0.1:18088/?model='+z.ALIAS+'-thinking')
        await browser.send('İki artı iki kaçtır?')
        await browser.wait_text('Yanıt tamamlanamadı')
        deadline = time.monotonic()+15
        while gateway.backend.turn.locked() and time.monotonic() < deadline:
            await asyncio.sleep(.2)
        assert not gateway.backend.turn.locked(), 'Cutoff turn did not finish'
        await browser.screenshot('ui-incomplete.png')
        incomplete = await browser.evaluate('document.body.innerText')
        assert '<think>' not in incomplete
        engine.close()
        await browser.send('Kısa yanıt ver.')
        await browser.wait_text('ZCI')
        deadline = time.monotonic()+30
        failed = ''
        while time.monotonic() < deadline:
            failed = await browser.evaluate('document.body.innerText') or ''
            if any(s in failed for s in ['bağlantı kurulamadı', 'modeliyle', '503', 'Service Unavailable']): break
            await asyncio.sleep(.5)
        else:
            (work/'ui-unavailable.txt').write_text(failed)
            raise RuntimeError('Unavailable model error not visible in UI')
        await browser.screenshot('ui-unavailable.png')
        return {'actual_firefox_ui': True, 'assistant_bubble': True, 'profile_selection': True,
                'incomplete_notice_visible': True, 'unavailable_notice_visible': True,
                'reasoning_not_mixed': True}
    finally:
        await browser.close()

def main():
    if '--run' not in sys.argv:
        raise SystemExit('Explicit --run required for the isolated live acceptance.')
    work = Path(tempfile.mkdtemp(prefix='zci-webui-live-'))
    print('ARTIFACTS', work, flush=True)
    (work/'home').mkdir()
    (work/'webui-data').mkdir(mode=0o700)
    base = Path(__file__).resolve().parent
    config = z.Config.load(base/'config.example.json')
    for p in [18086, 18087, 18088, 18092]:
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(('127.0.0.1', p))
    before = {}
    for p in [8087, 8088]:
        try:
            with OP.open(f'http://127.0.0.1:{p}/health', timeout=2) as r: before[str(p)] = r.status
        except OSError as error: before[str(p)] = str(error)
    report = {'existing_health_before': before, 'checks': {}, 'browser': {}, 'cleanup': {}}
    gateway = None; webui = None; log = None
    try:
        with z.Engine(config, work) as engine:
            gateway = z.Gateway(config).start()
            api_base = f'http://127.0.0.1:{config.gateway_port}'
            env = {'HOME': str(work/'home'), 'PATH': str(WEBUI/'venv/bin')+':/usr/bin:/bin',
                'LANG': 'C.UTF-8', 'ENV': 'prod', 'PYTHONDONTWRITEBYTECODE': '1', 'PYTHON_DOTENV_DISABLED': '1',
                'DATA_DIR': str(work/'webui-data'), 'DATABASE_URL': 'sqlite:///'+str(work/'webui-data/webui.db'),
                'FRONTEND_BUILD_DIR': str(WEBUI/'venv/lib/python3.12/site-packages/open_webui/frontend'),
                'STATIC_DIR': str(work/'webui-static'),
                'LICENSE_BLOB_PATH': str(work/'test-license'), 'WEBUI_AUTH': 'False',
                'WEBUI_SECRET_KEY': 'isolated-test-only-not-production', 'ENABLE_PERSISTENT_CONFIG': 'False',
                'ENABLE_OLLAMA_API': 'False', 'ENABLE_OPENAI_API': 'True',
                'OPENAI_API_BASE_URL': api_base+'/v1', 'OPENAI_API_BASE_URLS': api_base+'/v1',
                'OPENAI_API_KEY': 'unused-local', 'OPENAI_API_KEYS': 'unused-local',
                'DEFAULT_MODELS': z.ALIAS, 'DEFAULT_LOCALE': 'tr-TR',
                'DEFAULT_MODEL_METADATA': json.dumps({'capabilities': {'builtin_tools': False, 'vision': False, 'code_interpreter': False, 'web_search': False}}),
                'DEFAULT_MODEL_PARAMS': json.dumps({'max_tokens': 32}),
                'OFFLINE_MODE': 'True', 'HF_HUB_OFFLINE': '1', 'USE_SLIM_DOCKER': 'True',
                'DO_NOT_TRACK': 'true', 'SCARF_NO_ANALYTICS': 'true', 'ANONYMIZED_TELEMETRY': 'false',
                'ENABLE_VERSION_UPDATE_CHECK': 'False', 'ENABLE_CODE_EXECUTION': 'False',
                'ENABLE_CODE_INTERPRETER': 'False', 'ENABLE_WEB_SEARCH': 'False',
                'ENABLE_IMAGE_GENERATION': 'False', 'ENABLE_AUTOCOMPLETE_GENERATION': 'False',
                'ENABLE_FOLLOW_UP_GENERATION': 'False', 'ENABLE_TAGS_GENERATION': 'False',
                'ENABLE_TITLE_GENERATION': 'False', 'ENABLE_CONTEXT_COMPACTION': 'False',
                'ENABLE_MEMORIES': 'False', 'ENABLE_MEMORY_SYSTEM_CONTEXT': 'False',
                'ENABLE_MEMORY_BACKGROUND_REVIEW': 'False', 'RAG_EMBEDDING_ENGINE': 'openai',
                'FORWARDED_ALLOW_IPS': '127.0.0.1', 'NO_PROXY': 'localhost,127.0.0.1'}
            log = (work/'webui.log').open('w')
            webui = subprocess.Popen([str(WEBUI/'venv/bin/python'), '-B', '-m', 'uvicorn', 'open_webui.main:app',
                '--host', '127.0.0.1', '--port', '18088', '--workers', '1'], cwd=work,
                env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            wait_http('http://127.0.0.1:18088', '/health', webui)
            auth = http('http://127.0.0.1:18088', '/api/v1/auths/signin', {'email': 'test@example.invalid', 'password': 'synthetic'})
            token = auth['token']
            models = http('http://127.0.0.1:18088', '/api/models', token=token)
            ids = [m['id'] for m in models['data']]
            assert z.ALIAS in ids and z.ALIAS+'-thinking' in ids and 'zci-qwen3-1.7b' not in ids
            report['checks']['isolated_model_routing'] = ids
            for alias, maximum in [(z.ALIAS, 32), (z.ALIAS+'-thinking', 1)]:
                http('http://127.0.0.1:18088', '/api/v1/models/create', {'id': alias, 'base_model_id': None,
                    'name': alias, 'meta': {'capabilities': {'builtin_tools': False, 'vision': False,
                    'code_interpreter': False, 'web_search': False}}, 'params': {'max_tokens': maximum}}, token)
            real_webui = http('http://127.0.0.1:18088', '/api/chat/completions', {'model': z.ALIAS,
                'messages': [{'role': 'user', 'content': 'Yalnız TAMAM yaz.'}], 'stream': False}, token)
            assert real_webui['choices'][0]['message']['content'].strip()
            report['checks']['real_webui_json'] = True
            report['checks']['real_model_json'] = {'usage': real_webui.get('usage'), 'through_webui': True}
            report['browser'] = asyncio.run(browser_checks(work, token, engine, gateway))
            del token, auth
    finally:
        stop(webui)
        if gateway: gateway.close()
        if log: log.close()
        report['cleanup']['webui_stopped'] = webui is None or webui.poll() is not None
        report['cleanup']['engine_stopped'] = 'engine' not in locals() or engine.process is None or engine.process.poll() is not None
        report['cleanup']['active_upstreams'] = len(gateway.backend.active) if gateway else 0
        after = {}
        for p in [8087, 8088]:
            try:
                with OP.open(f'http://127.0.0.1:{p}/health', timeout=2) as r: after[str(p)] = r.status
            except OSError as error: after[str(p)] = str(error)
        report['existing_health_after'] = after
        report['cleanup']['existing_health_unchanged'] = before == after
        (work/'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
        print('RESULT', work/'result.json', flush=True)

if __name__ == '__main__': main()

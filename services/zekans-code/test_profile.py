import ast
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
from task_guard import assess
import zekans_code as z


class GuardTests(unittest.TestCase):
    def test_unspecified_objectives_ask_real_question(self):
        for message in ['Bu uygulamayı daha iyi hale getir.', 'Lütfen kodu iyileştir.',
                        'main.py performansını optimize et.', 'Bu kodu güzelleştir.',
                        'module2.py daha güvenli hale getir.', 'Improve this application.',
                        'Optimize main.py.', 'Hatayı düzelt.', 'main.py hatayı düzelt.',
                        'main.py dosyasını daha iyi hale getir, mevcut arayüzü koru.',
                        'kodu sadeleştir.', 'kodu temizle.', 'uygulamayı refactor et.',
                        'hataları düzelt.', 'sorunları çöz.', 'sorunu gider.', '']:
            with self.subTest(message=message):
                decision=assess(message)
                self.assertFalse(decision.proceed)
                self.assertTrue(decision.question.endswith('?'))
                self.assertNotIn('soracağım',decision.question)

    def test_explicit_conflicts_ask_without_selecting_requirement(self):
        for message in ['JSON üret. JSON kullanma.', 'Sonucu yazdır; hiç çıktı üretme.',
                        'Tam iki cümle yaz. Tam üç cümle yaz.',
                        "Şimdi main.py'yi düzenle. JSON üret ve JSON üretme.",
                        "main.py'de JSON üret ve JSON üretme; README.md'ye dokunma.",
                        'Önce JSON üret demiştim. Şimdi JSON üret ve JSON üretme.']:
            with self.subTest(message=message):
                self.assertFalse(assess(message).proceed)
                self.assertTrue(assess(message).question.endswith('?'))

    def test_concrete_requests_and_negative_rules_are_not_blocked(self):
        messages=[
            'Yalnız main.py oluştur. İki tamsayı okuyup toplamı yazdır.',
            'clamp(value, low, high) aralıktaysa value, altındaysa low, üstündeyse high dönsün.',
            'İki dosyada mean aritmetik ortalama döndürsün; sonucu iki ondalıkla yazdır.',
            'n dahil toplamı döndür, n <= 0 durumunda 0 döndür.',
            'JSON kullanma. Yalnız düz metin ver.',
            'JSON üretme; sonucu yazdır.',
            'JSON yerine düz metin yazdır.',
            'Önce JSON üret demiştim. Şimdi JSON kullanma; metin yazdır.',
            'Tam iki cümle yerine tam üç cümle yaz.',
            'Tam 2 cümle yaz. Tam iki cümle yaz.',
            'Kodu daha güvenli hale getir; parametrik sorgu kullan.',
            'Kodu iyileştir; açıklayıcı yorum ekle.',
            'main.py performansını iyileştir; 1000 kayıtla 2 saniyeden kısa sürmeli.',
            'Hata mesajlarını Türkçeleştir, davranışı koru.',
            'Kodu iyileştir; hata mesajlarını Türkçeleştir, davranışı koru.',
            'In sample.py, replace "before" with "after"; keep GREETING unchanged.',
            'Program "JSON üret; JSON kullanma" metnini yazdır.',
            'main.py içinde "çıktı üretme" yerine "sonucu yazdır" metnini kullan.',
            'kodu sadeleştir; gereksiz x değişkenini kaldır.',
            'hataları düzelt; sıfıra bölme durumunda 0 döndür.',
            'sorunu gider; n <= 0 durumunda boş liste dönsün.',
        ]
        for message in messages:
            with self.subTest(message=message):self.assertTrue(assess(message).proceed)
        self.assertTrue(assess('Bu uygulamayı daha iyi hale getir.','İki tamsayıyı okuyup toplamını tek satırda yazdırır.').proceed)
        self.assertFalse(assess('JSON üret.','JSON kullanma.').proceed)


@unittest.skipUnless(Path(z.config_load(HERE/'profile.json')['agent_base'],'runner.py').exists(),
                     "Installed agent runner outside sandbox required for full adapter test")
class AdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.config=z.config_load(HERE/'profile.json')
    def test_fixed_source_pins_and_preserved_audit(self):
        source=z.runner_source(self.config)
        old=Path(self.config['agent_base'],'runner.py').read_text()
        def function(text,name):
            return next(n for n in ast.parse(text).body if isinstance(n,(ast.FunctionDef,ast.ClassDef)) and n.name==name)
        normalized=source.replace(str(self.config['gateway_port']),'8087')
        for name in ('audit','source_path','no_shell','restricted_do_run','VerificationClient','report_result'):
            self.assertEqual(ast.dump(function(old,name)),ast.dump(function(normalized,name)))
        self.assertIn('Coder.max_reflections = 1',source)
        self.assertIn('not in EDITABLE',source)
        self.assertIn('ast.parse(content',source)
        self.assertIn("self.extra_params != {'temperature': 0.2, 'max_tokens': 768}",source)
    def test_changed_runner_pin_fails_closed(self):
        cfg=dict(self.config,runner_sha256='0'*64)
        with self.assertRaises(ValueError):z.runner_source(cfg)
    def test_command_only_replaces_owned_state_and_runner_mount(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);project=root/'project';project.mkdir();(project/'.git').mkdir();work=root/'work';work.mkdir()
            command=z.build_command(self.config,project,['main.py'],'main.py oluştur ve toplamı yazdır.',{},work)
            self.assertIn('--disable-userns',command);self.assertIn('--cap-drop',command)
            self.assertIn('--remount-ro',command)
            index=command.index('--setenv',command.index('--setenv')+1)
            self.assertIn(str(work/'restricted-runner.py'),command)
            self.assertIn(str(work/'state'),command)
            self.assertIn(str(project/'.git'),command)
            self.assertNotIn('http://127.0.0.1:8087/v1',command)


class ProxyTests(unittest.TestCase):
    def setUp(self):
        self.proxy=object.__new__(z.Proxy);self.proxy.config={'alias':'zekans-qwen3-4b'}
        self.body={'model':'zekans-qwen3-4b','messages':[{'role':'user','content':'toplamı yazdır'}],
                   'max_tokens':768,'temperature':.2,'stream':True}
    def test_effective_profile(self):
        payload=self.proxy.prepare(self.body)
        self.assertEqual(payload['seed'],7);self.assertEqual(payload['top_k'],40)
        self.assertEqual(payload['chat_template_kwargs'],{'enable_thinking':False})
        self.assertFalse(payload['cache_prompt'])
    def test_tools_model_fallback_and_output_override_rejected(self):
        for additions in [{'tools':[]},{'model':'zci-qwen3-1.7b'},{'max_tokens':1024},
                          {'temperature':.6},{'chat_template_kwargs':{'enable_thinking':True}}]:
            with self.subTest(additions=additions):
                with self.assertRaises(ValueError):self.proxy.prepare(dict(self.body,**additions))


class MonitorTests(unittest.TestCase):
    def test_lost_observation_stops_only_owned_process(self):
        engine=object.__new__(z.Engine);engine.stop=Mock();engine.stop.wait.return_value=False
        engine.process=Mock();engine.process.poll.return_value=None;engine.abort=None
        engine.stats=Mock(side_effect=RuntimeError('proc unavailable'));engine.samples=[]
        engine.monitor_loop()
        engine.process.terminate.assert_called_once_with()
        self.assertEqual(engine.abort,'RAM monitoring unavailable')
    def test_exited_process_is_normal(self):
        engine=object.__new__(z.Engine);engine.stop=Mock();engine.stop.wait.return_value=False
        engine.process=Mock();engine.process.poll.return_value=0;engine.abort=None
        engine.monitor_loop();engine.process.terminate.assert_not_called()

    def test_portable_profile_paths_expand_user_and_have_no_hardcoded_home(self):
        self.assertNotIn('/home/' + 'Zevor',(HERE/'profile.json').read_text())
        self.assertNotIn('/home/' + 'Zevor',(HERE/'zekans_code.py').read_text())
        cfg=z.config_load(HERE/'profile.json')
        expected=str(Path.home()/'.local'/'share'/'zci')
        for key in ('engine','model','agent_base'):
            self.assertFalse(cfg[key].startswith('~'))
            self.assertTrue(cfg[key].startswith(expected))


if __name__=='__main__':unittest.main()

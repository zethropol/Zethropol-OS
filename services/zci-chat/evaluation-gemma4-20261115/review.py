"""Bind an external rubric review to the measured raw answers; no model grading."""
import hashlib
import json
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent
# semantic: pass/partial/fail; instruction: pass/fail. Partial never counts as pass.
GRADES = {
    'gemma4': [
        ('pass', 'fail', 'Yorgunluğa uygun karşılık var; kendini tanıtıyor, dört cümle üretiyor.'),
        ('pass', 'fail', 'İş arkadaşına uygun açılış öneriyor; istenmeyen giriş ve açıklama ekliyor.'),
        ('pass', 'pass', 'Yeni tanışılan iş arkadaşını tek cümlede doğru hatırlıyor.'),
        ('pass', 'pass', 'Kaydettim. birebir doğru.'),
        ('pass', 'pass', 'En son gün Cuma; yalnız doğru ad.'),
        ('pass', 'pass', '4 × 3 = 12; yalnız sayı.'),
        ('pass', 'pass', 'Üyelik çıkarımını reddediyor ve alternatif yolu iki cümlede açıklıyor.'),
        ('pass', 'pass', 'Teknik özellik verilmediğini iki cümlede açıklıyor, boyut uydurmuyor.'),
        ('pass', 'pass', 'Üç farklı pratik cümle; liste veya kapanış yok.')
    ],
    'qwen3-4b': [
        ('partial', 'fail', 'Konuşma isteğine karşılık var; “Ne hakkında konuşmak ister misin?” bozuk, kendini tanıtıyor, dört cümle/iki soru var.'),
        ('fail', 'fail', 'Açılış örneği yerine kullanıcıya iki soru yöneltiyor; aynı bozuk soru kalıbı tekrarlanıyor.'),
        ('partial', 'pass', 'İş arkadaşı ifadesi var, fakat geçmişi bildiren cevap yerine konuşmak isteyip istemediğini soruyor.'),
        ('pass', 'pass', 'Kaydettim. birebir doğru.'),
        ('fail', 'fail', 'Son düzeltme cuma olmasına rağmen Çarşamba. diyor; exact denetimi de geçmiyor.'),
        ('fail', 'pass', 'Yalnız sayı biçimi doğru; 12 yerine 16 hesaplıyor. exact denetimi başarısız.'),
        ('fail', 'fail', 'Geçersiz ters çıkarımı kabul ediyor; iki yerine üç cümle var.'),
        ('pass', 'pass', 'Teknik detaylar verilmediği için boyutun bilinemeyeceğini açıklıyor.'),
        ('partial', 'fail', 'Uygulanabilir öneriler var; “Bu, dikkatini toplar ve ... okuyabilirsin” uyumsuz, “anlaştığın noktalar” yanlış sözcük seçimi. Yasaklanan liste ve altı cümle var.')
    ]
}


def main():
    summary = {}
    reviews = {}
    common = None
    for name, directory in [('gemma4', ROOT), ('qwen3-4b', ROOT / 'qwen3-4b')]:
        raw = (directory / 'responses.json').read_bytes()
        rows = json.loads(raw)
        corpus = json.loads((directory / 'acceptance-frozen.json').read_text())
        turns = corpus['cases'][0]['turns']
        assert len(rows) == len(turns) == 9
        history = [rows[0]['messages'][0]]
        review = []
        for index, (row, turn, grade) in enumerate(zip(rows, turns, GRADES[name]), 1):
            history.append({'role': 'user', 'content': turn['user']})
            assert row['messages'] == history, 'History was altered'
            assert row['id'] == turn['id'] and row['turn'] == index
            assert row['finish_reason'] == 'stop'
            assert row['server_timings']['cache_n'] == 0
            assert row['input_tokens'] + 768 <= 32768
            history.append({'role': 'assistant', 'content': row['answer']})
            semantic, instruction, reason = grade
            review.append({'id': row['id'], 'answer': row['answer'], 'semantic': semantic,
                           'instruction': instruction, 'full_pass': semantic == instruction == 'pass',
                           'critical_hallucination_events': 0, 'reason': reason})
        settings = json.loads((directory / 'settings.json').read_text())
        defaults = json.loads((directory / 'props.json').read_text())['default_generation_settings']['params']
        effective = dict(defaults)
        effective.update(temperature=.2, seed=7, max_tokens=768)
        if name == 'qwen3-4b':
            effective.update(top_k=64, top_p=defaults['top_p'], min_p=defaults['min_p'])
        # These are all actual sampler defaults plus request overrides, not merely requested labels.
        comparable = {k: effective[k] for k in ('temperature', 'seed', 'top_k', 'top_p', 'min_p',
                      'samplers', 'repeat_penalty', 'presence_penalty', 'frequency_penalty',
                      'dry_multiplier', 'typical_p', 'mirostat', 'max_tokens')}
        comparable.update(system_sha256=settings['system_sha256'], context=settings['context'],
                          threads=settings['threads'], cache_prompt=settings['cache_prompt'], thinking=False,
                          corpus_sha256=json.loads((directory / 'corpus-provenance.json').read_text())['sha256'])
        if common is None:
            common = comparable
        else:
            assert common == comparable, 'Effective comparison settings differ'
        lifecycle = json.loads((directory / 'lifecycle.json').read_text())
        assert lifecycle['stopped'] and lifecycle['resource_abort'] is None
        assert json.loads((directory / 'protected-after.json').read_text())['unchanged']
        reviews[name] = {'responses_sha256': hashlib.sha256(raw).hexdigest(), 'turns': review,
                         'critical_hallucination_note': 'İki koşuda da boyut uydurma veya gerçek sistem işlemi iddiası görülmedi.'}
        summary[name] = {
            'full_success': sum(r['full_pass'] for r in review),
            'semantic_success': sum(r['semantic'] == 'pass' for r in review),
            'semantic_partial': sum(r['semantic'] == 'partial' for r in review),
            'turns': 9, 'critical_hallucination_events': 0, 'critical_hallucination_turns': 0,
            'median_response_seconds': statistics.median(r['total_seconds'] for r in rows),
            'median_first_content_seconds': statistics.median(r['first_content_seconds'] for r in rows),
            'median_server_generation_tokens_per_second': statistics.median(r['server_timings']['predicted_per_second'] for r in rows),
            'peak_rss_gib': lifecycle['peak_rss_kib'] / 1024**2,
            'peak_hwm_gib': lifecycle['peak_hwm_kib'] / 1024**2,
            'minimum_available_gib': lifecycle['minimum_available_bytes'] / 1024**3,
            'input_token_range': [min(r['input_tokens'] for r in rows), max(r['input_tokens'] for r in rows)],
            'total_generated_tokens': sum(r['usage']['completion_tokens'] for r in rows),
            'strict_acceptance': all(r['full_pass'] for r in review)
        }
    (ROOT / 'review.json').write_text(json.dumps(reviews, ensure_ascii=False, indent=2) + '\n')
    report = {'common_effective_settings': common, 'models': summary,
              'same_history_protocol_confirmed': True,
              'preliminary_qwen_topk40_excluded': True,
              'token_rate_definition': 'Median of server predicted_per_second, including generated special tokens; transport-observed rate is retained separately in raw rows.',
              'ttft_definition': 'Client request to first nonempty content delta; parser/network buffering may differ from first internally decoded token.',
              'context_limit': '32768 allocated; actual input ranges reported. No full 32K inference or general long-context quality acceptance.',
              'cgroup_security_tests': 'Five outstanding tests remain open; not run or accepted here.'}
    (ROOT / 'comparison.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()

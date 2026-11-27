"""Validate the frozen nine-turn corpus and known hallucination regressions."""
import hashlib
import json
from pathlib import Path
import re


def unsupported_header_size(answer):
    """Narrow negative-fixture detector, not an automatic semantic grader."""
    return bool(re.search(r'\b(?:12|928)\s*bayt', answer.casefold()))


def main():
    path = Path(__file__).with_name('acceptance.json')
    data = json.loads(path.read_text())
    assert data['schema'] == 'zci-turkish-conversation-acceptance-v1'
    assert data['version'] == '1.0.0'
    assert len(data['cases']) == 1
    turns = data['cases'][0]['turns']
    assert len(turns) == 9 and len({t['id'] for t in turns}) == 9
    for turn in turns:
        assert turn['user'].strip() and turn['expected'].strip() and turn['checks']
    coverage = {check for turn in turns for check in turn['checks']}
    assert {'natural_turkish', 'intent', 'history', 'latest_correction', 'exact',
            'arithmetic', 'logic', 'uncertainty', 'no_repetition'} <= coverage
    assert {t['id']: t['exact'] for t in turns if 'exact' in t} == {
        'update': 'Kaydettim.', 'latest_context': 'Cuma', 'arithmetic': '12'}
    negatives = data['negative_regressions']
    assert len(negatives) == 2
    for negative in negatives:
        assert negative['turn_id'] == 'unknown'
        assert negative['semantic_pass'] is False and negative['critical_hallucination'] is True
        assert unsupported_header_size(negative['answer']), 'Known hallucination escaped fixture detector'
    assert not unsupported_header_size('Teknik özelliklerini vermediğin için başlık uzunluğunu bilmiyorum.')
    print(json.dumps({'schema_valid': True, 'turns': 9, 'negative_regressions_rejected': 2,
                      'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}, indent=2))


if __name__ == '__main__':
    main()

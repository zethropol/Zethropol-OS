"""Conservative, local request checks. Never reads or edits project contents."""
from dataclasses import dataclass
import re
import unicodedata


def normalized(text):
    return unicodedata.normalize('NFKC', text).replace('İ', 'i').casefold()


@dataclass(frozen=True)
class Decision:
    proceed: bool
    reason: str = ''
    question: str = ''


def assess(message, expected_behavior=None):
    """Catch unsupported objectives and explicit contradictions, without guessing.

    This is a bounded preflight, not proof that arbitrary language is consistent.
    Source-code fences are data; quoted replacement text is not interpreted as
    an instruction. Concrete requests stay on the normal Aider edit path.
    """
    if not isinstance(message, str) or not message.strip():
        return Decision(False, 'missing_task', 'Hangi değişikliği yapmamı istiyorsunuz?')
    if expected_behavior is not None and not isinstance(expected_behavior,str):
        raise ValueError('Expected behavior must be user-supplied text')
    task = message + ('\nBeklenen davranış: '+expected_behavior if expected_behavior else '')
    text = normalized(re.sub(r'```.*?```', '', task, flags=re.S))
    # Turkish suffix apostrophes (main.py'de) are word punctuation, not quotes.
    instructions = re.sub(r'(?<!\w)(["\']).*?\1(?!\w)', ' ÖRNEK ', text)
    # Only an explicitly retrospective correction drops an earlier clause.
    # "Şimdi" alone must never disable conflict checking for a new request.
    correction=re.search(r'\b(?:demiştim|istemiştim|söylemiştim)\b[.!;\s]*(?:şimdi|artık)\b',instructions)
    if correction:
        instructions=instructions[correction.end():]
    positive_json = re.search(r'\bjson\s+(?:üret|yaz|döndür|oluştur|ver|output|return)\b', instructions)
    negative_json = re.search(r'\bjson\s+(?:kullanma|üretme|yazma|döndürme|oluşturma|verme)\b', instructions)
    if positive_json and negative_json:
        return Decision(False, 'conflicting_format', 'Çıktı JSON mu olmalı, yoksa JSON kullanılmamalı mı?')
    no_output = re.search(r'\b(?:hiç\s+)?çıktı\s+(?:üretme|yazma|verme)|\bhiçbir şey yazdırma', instructions)
    positive_output = re.search(r'\b(?:sonucu|toplamı|değeri|metni|sayıyı)\s+yazdır\b', instructions)
    if no_output and positive_output:
        return Decision(False, 'conflicting_output', 'Program sonucu yazdırmalı mı, yoksa hiç çıktı üretmemeli mi?')
    count_text=re.sub(r'\btam\s+(?:bir|iki|üç|dört|beş|\d+)\s+cümle\s+yerine\s*','',instructions)
    counts = re.findall(r'\btam\s+(bir|iki|üç|dört|beş|\d+)\s+cümle\b', count_text)
    number_words={'bir':1,'iki':2,'üç':3,'dört':4,'beş':5}
    count_values={number_words[value] if value in number_words else int(value) for value in counts}
    if len(count_values) > 1:
        return Decision(False, 'conflicting_count', 'Yanıt tam olarak kaç cümle olmalı?')
    # Vague optimization/refactoring requests lack a testable change objective.
    vague = re.search(r'\b(?:iyileştir|geliştir|güzelleştir|hızlandır|optimize et|daha iyi hale getir|daha güvenli hale getir|temizle|sadeleştir|refactor|improve|optimize|make it better)\b', instructions)
    concrete = re.search(r'\b(?:yerine|olunca|durumunda|döndür|dönsün|yazdır|okuyup|okuyarak|sıral|toplam|ortalama|ondalık|stdout|stdin|return|replace|input|output|raises?|error|hata mesajı)\b|\d|=>|→', instructions)
    # File names and version numbers alone are not a concrete objective.
    no_paths = re.sub(r'\b[\w./-]+\.(?:py|js|ts|rs|cpp|h|qml|json|md|txt)\b', '', instructions)
    concrete_without_paths = re.search(r'\b(?:yerine|olunca|durumunda|döndür\w*|dönsün|yazdır\w*|okuyup|okuyarak|sıral\w*|toplam\w*|ortalama\w*|ondalık|stdout|stdin|return|replace|input|output|raises?|error|hata mesajı|ekle|kaldır|sil|ayır|birleştir|değiştir|türkçeleştir|çevir|yerelleştir)\b|=>|→', no_paths)
    new_query_mechanism=re.search(r'\b(?:parametrik|parametreli|hazırlanmış)\s+sorgu\s+kullan\b',no_paths)
    measurable = re.search(r'\d+(?:[.,]\d+)?\s*(?:ms|milisaniye|saniye|sn|dakika|kb|kib|mb|mib|gb|gib|bayt|byte|kayıt|satır|records?|seconds?|milliseconds?)(?:den|dan|de|da|la|le|lık|lik|li|lı)?\b',no_paths)
    if vague and not concrete_without_paths and not measurable and not new_query_mechanism:
        return Decision(False, 'unspecified_improvement', 'Hangi davranışı değiştirmemi veya hangi sorunu düzeltmemi istiyorsunuz?')
    generic_fix = re.fullmatch(r'[\s\w./-]*\b(?:hatayı düzelt|hataları düzelt|sorunu çöz|sorunları çöz|sorunu gider|sorunları gider|fix (?:it|the bug|the error|the errors))[.!\s]*', instructions)
    if generic_fix and not concrete_without_paths:
        return Decision(False, 'missing_expected_behavior', 'Beklenen davranış nedir ve şu anda hangi hata oluşuyor?')
    return Decision(True)

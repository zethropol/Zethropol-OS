"""Build immutable audit-first reader; retain original transaction/bundle verbatim."""
import ast
import base64
import hashlib
import json
from pathlib import Path

BASE=Path(__file__).resolve().parents[1]
ORIGINAL=BASE/'build/aur-agent-home-lifecycle-v2-files-transaction-20261103'
TXID='57652e06b38ac2bd6d153ce5d217b3d9'
MANIFEST='c1c90688a452cb8e74871f44696afdfe1299d290a12ec73b53421e8fe4503992'
BUNDLE_SHA='e9325b0b8b49d53bf5528526963cf1cde56ca2a75007e15dbe4ab01cff9746d9'
DESTINATION=BASE/('build/aur-agent-home-lifecycle-v2-partial-review-'+TXID)

def sha(raw):return hashlib.sha256(raw).hexdigest()

def prepare(destination=DESTINATION):
    raw=(ORIGINAL/'file-transaction.pinned.py').read_bytes()
    if sha(raw)!=BUNDLE_SHA:raise ValueError('historical bundle changed')
    tree=ast.parse(raw);bundle=ast.literal_eval(tree.body[0].value)
    source=ast.literal_eval(tree.body[1].value.args[0].args[0])
    manifest_raw=(ORIGINAL/'deployment-manifest.INACTIVE.json').read_bytes()
    if sha(manifest_raw)!=MANIFEST:raise ValueError('historical manifest changed')
    wrapper=(ORIGINAL/('apply-'+TXID+'.fish')).read_text()
    quoted=wrapper.split(' -c ',1)[1].rsplit('\nexit $status',1)[0].strip()
    code=quoted[1:-1].replace("\\'","'").replace('\\\\','\\')
    found=[]
    for node in ast.walk(ast.parse(code)):
        if isinstance(node,ast.Dict):
            values={ast.literal_eval(k):ast.literal_eval(v) for k,v in zip(node.keys,node.values) if isinstance(v,ast.Constant)}
            if values.get('MODE')=='apply':found.append(values)
    if len(found)!=1 or found[0]['BUNDLE_SHA']!=BUNDLE_SHA:raise ValueError('original capture bootstrap binding')
    envelope=json.loads(base64.b64decode(found[0]['CAPTURE_B64'],validate=True))
    if envelope['transaction_id']!=TXID or envelope['capture']['manifest_sha256']!=MANIFEST:raise ValueError('original transaction capture')
    bound={'envelope':envelope,'manifest':bundle['manifest'],'manifest_sha256':MANIFEST,'bundle_sha256':BUNDLE_SHA}
    review_source=(BASE/'aur/home_lifecycle_partial_readonly.py').read_text()
    reader_raw=('import types,os\ntx=types.ModuleType("historical_inert_transaction")\n'
                +'exec(compile('+repr(source)+',"historical-inert-transaction","exec"),tx.__dict__)\n'
                +'tx.D|=os.O_NOATIME\nBOUND='+repr(bound)+'\n'
                +'exec(compile('+repr(review_source)+',"partial-review","exec"),globals())\n').encode()
    destination.mkdir(exist_ok=True);reader=destination/'partial-review.pinned.py';reader.write_bytes(reader_raw)
    # Bootstrap only; source definitions are extracted from the pinned old bytes.
    module={'__name__':'historical_readonly_bootstrap_definitions'};exec(compile(source,'bootstrap-definitions','exec'),module)
    command='/usr/bin/sudo /usr/bin/python3 -I -S -B -c '+module['fish_quote'](module['bootstrap'](str(reader),sha(reader_raw),'readonly-partial-review'))
    (destination/'review-partial.fish').write_text('#!/usr/bin/fish\n# READ ONLY: audit first; no apply/rollback/adoption.\nif not test -t 0\n echo "BLOCKED: local terminal required"\n exit 1\nend\n'+command+'\nexit $status\n')
    (destination/'review-pins.json').write_text(json.dumps({'reader_sha256':sha(reader_raw),'historical_bundle_sha256':BUNDLE_SHA,
                    'historical_manifest_sha256':MANIFEST,'original_transaction_id':TXID,'host_changed':False},indent=2)+'\n')
    return bound

if __name__=='__main__':prepare();print(DESTINATION)

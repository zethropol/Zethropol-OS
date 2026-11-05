"""Generate a separate self-contained preflight/apply candidate in workspace."""
import base64
import json
from pathlib import Path
import prepare_home_lifecycle_files as files
import home_lifecycle_file_transaction as transaction

DESTINATION=files.BASE/'build/aur-agent-home-lifecycle-v2-files-transaction-20261103'

def prepare(destination=DESTINATION):
    manifest=files.prepare(destination)
    manifest['status']='PREPARED_PENDING_REAL_FILE_DEPLOYMENT_PREFLIGHT'
    manifest['host_evidence']={'status':'USER_READONLY_SOCKET_SAFETY_CAPTURED','visibility':'INCOMPLETE',
                              'global_holder_absence_proven':False,'maintenance_authority':False,
                              'socket_objects':{n:{'device':38,'inode':i,'ctime_ns':1793481059412486406,'uid':949,'gid':948,
                                                   'mode':'0700','nlink':1,'acl':{},'type':'socket'} for n,i in transaction.SOCKET_INODES.items()}}
    manifest['required_checks'].append('root-protected staging for actor directory; never stage/chown entries in owner-writable HOME')
    manifest['future_records']['deployment_journal']['path_template']=files.STATE+'/deployment/<new-128bit-id>/{claim,manifest,preflight,postflight,rollback}.json'
    raw=transaction.canonical(manifest)
    (destination/'deployment-manifest.INACTIVE.json').write_bytes(raw)
    payload={f['target']:base64.b64encode(Path(f['source']).read_bytes()).decode() for f in manifest['files']}
    bundle={'manifest':manifest,'manifest_sha256':transaction.sha(raw),'payload':payload}
    source=(files.SOURCE/'home_lifecycle_file_transaction.py').read_text()
    candidate=destination/'file-transaction.pinned.py'
    candidate.write_text('BUNDLE='+repr(bundle)+'\nexec(compile('+repr(source)+',"home_lifecycle_file_transaction","exec"),globals())\n')
    digest=transaction.sha(candidate.read_bytes())
    command='/usr/bin/sudo /usr/bin/python3 -I -S -B -c '+transaction.fish_quote(transaction.bootstrap(str(candidate),digest,'preflight'))
    (destination/'preflight-files.fish').write_text('#!/usr/bin/fish\n# READ ONLY. PASS prints a separate files-only apply command; never applies automatically.\nif not test -t 0\n echo "BLOCKED: local terminal required"\n exit 1\nend\n'+command+'\nexit $status\n')
    (destination/'transaction-pins.json').write_text(json.dumps({'manifest_sha256':transaction.sha(raw),'bundle_sha256':digest,
                    'executor_source_sha256':transaction.sha(source.encode()),'host_applied':False,'activation_approved':False,
                    'quarantine_authorized':False},indent=2)+'\n')
    return bundle

if __name__=='__main__':prepare();print(DESTINATION)

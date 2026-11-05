"""Build workspace-only inert deployment manifest and pinned readonly Fish reader."""
import hashlib
import json
from pathlib import Path
import shutil

SOURCE=Path(__file__).resolve().parent
BASE=SOURCE.parent
CANDIDATE=BASE/'build/aur-agent-home-lifecycle-v2-files-only-20261103'
PREVIOUS=BASE/'build/aur-agent-home-lifecycle-v2-mode-policy-20261103'
RELEASE='/usr/lib/zethropol/aur-agent-lifecycle-949/releases/home-route-v2-20261103'
STATE='/var/lib/zethropol-aur-agent-lifecycle-949'
ACTOR='/var/lib/zethropol-aur-release/agent-lifecycle-v2'
UNIT='/etc/systemd/system/zethropol-aur-manifest-agent.service'

def sha(raw):return hashlib.sha256(raw).hexdigest()
def encoded(obj):return (json.dumps(obj,indent=2,sort_keys=True)+'\n').encode()

def prepare(destination=CANDIDATE):
    destination.mkdir(exist_ok=True)
    names=('agent_lifecycle_949.py','native_socket_route_readonly.py')
    for name in names:shutil.copyfile(SOURCE/name,destination/name)
    unit=SOURCE/'release/providers'/Path(UNIT).name
    shutil.copyfile(unit,destination/unit.name)
    policy=json.loads((PREVIOUS/'policy.UNAPPROVED.json').read_text())
    policy.update(activation_approved=False,activation_id=None,quarantine_receipt=None)
    policy['helper_sha256']=sha((destination/names[0]).read_bytes())
    policy['route_reader_sha256']=sha((destination/names[1]).read_bytes())
    policy['unit_sha256']=sha(unit.read_bytes())
    (destination/'policy.INACTIVE.json').write_bytes(encoded(policy))
    (destination/'lifecycle.lock.EMPTY').write_bytes(b'')
    files=[]
    for name,target in [(n,RELEASE+'/'+n) for n in names]+[('policy.INACTIVE.json',RELEASE+'/policy.json'),(unit.name,UNIT),('lifecycle.lock.EMPTY',ACTOR+'/lifecycle.lock')]:
        files.append({'source':str(destination/name),'target':target,'uid':949 if target.startswith(ACTOR) else 0,
                      'gid':948 if target.startswith(ACTOR) else 0,'mode':'0600' if target.startswith(ACTOR) else '0644',
                      'type':'regular','nlink':1,'acl':{},'sha256':sha((destination/name).read_bytes()),'preimage':'ABSENT_REQUIRED'})
    directories=[]
    for path,uid,gid,mode in ((str(Path(RELEASE).parent.parent),0,0,'0755'),(str(Path(RELEASE).parent),0,0,'0755'),
                              (RELEASE,0,0,'0755'),(STATE,0,0,'0755'),(STATE+'/quarantine',0,0,'0700'),
                              (STATE+'/deployment',0,0,'0700'),
                              ('/etc/systemd/system/.zethropol-aur-lifecycle-v2-rollback',0,0,'0700'),(ACTOR,949,948,'0700')):
        directories.append({'target':path,'uid':uid,'gid':gid,'mode':mode,'type':'directory','acl':{},
                            'sha256':None,'hash_semantics':'no directory byte hash; pin dev/inode/ctime and exact metadata during preflight',
                            'preimage':'ABSENT_REQUIRED' if path in (RELEASE,STATE,STATE+'/quarantine',STATE+'/deployment',ACTOR,'/etc/systemd/system/.zethropol-aur-lifecycle-v2-rollback') else 'ABSENT_OR_EXACT_SAFE_METADATA'})
    protected_plan=BASE/'build/aur-owner-949-final-distribution-20261101/distribution-plan.json'
    protected_raw=protected_plan.read_bytes()
    if sha(protected_raw)!='2ef3c2471498a8c86e09c28ca6732f6bc231d65ef9f1a915768f1cff8ef53711':raise ValueError('protected plan changed')
    protected=[{k:f[k] for k in ('target','mode','sha256')} for f in json.loads(protected_raw)['files']]
    if len(protected)!=13:raise ValueError('thirteen protected files required')
    manifest={'schema':1,'purpose':'home-lifecycle-v2-inert-files-only','status':'PREPARED_NOT_HOST_PREFLIGHTED',
              'host_applied':False,'activation_approved':False,'quarantine_authorized':False,'production_enabled':False,
              'files':files,'directories':directories,'protected_signer_baseline':protected,
              'protected_baseline_note':'accepted plan hashes are preservation expectations, never authority to repair mismatch',
              'preserve':['all existing GNUPGHOME objects including primary and auxiliary sockets','gpg-agent.conf exact metadata and bytes','all legacy audits/approvals/pending/nonces'],
              'future_records':{'deployment_journal':{'path_template':STATE+'/deployment/<new-128bit-id>/{manifest,preflight,events,postflight,rollback}.json',
                                  'uid':0,'gid':0,'mode':'0600','type':'regular','nlink':1,'acl':{},'sha256':None,
                                  'hash_semantics':'future variable records; compute SHA256 after each sealed record, never invent before capture'},
                                'actor_events':'NONE at file deployment; activation-only O_EXCL UID949:GID9480600 *.jsonl/start.*.used',
                                'root_claim_and_receipt':'NONE at file deployment; separate maintenance/activation authority',
                                'retained_unit_on_rollback':{'path_template':'/etc/systemd/system/.zethropol-aur-lifecycle-v2-rollback/<new-128bit-id>.unit',
                                                             'uid':0,'gid':0,'mode':'0644','nlink':1,'acl':{},'sha256':sha(unit.read_bytes())}},
              'required_checks':['fresh nofollow ancestry/target absence and no unit/dropin/transient/enable collision',
                                 'unit inactive and not queued; inhibit concurrent privileged deployment/start for publication window',
                                 'protected 13 files/config/socket identities unchanged before/after; no private key content reads',
                                 'inactive policy exact bytes and hashes; null activation ID and receipt',
                                 'root-only staging same filesystem; fsync files and parents; release no-replace, unit no-replace last'],
              'commands_excluded':['gpg','gpgconf','systemctl start','systemctl enable','systemctl daemon-reload','socket rename/unlink/chmod/chown'],
              'atomicity':'complete release directory renameat2 RENAME_NOREPLACE; unit commit last; not cross-filesystem global atomicity',
              'host_evidence':{'status':'user READONLY_BINDINGS_CAPTURED','config_hash_match':True,'deployment_targets':'ABSENT as reported; fresh preflight required',
                               'primary':policy['old_socket'],'auxiliary':{'path':'/var/lib/zethropol-aur-release/gnupg/S.gpg-agent.ssh','device':'38','inode':'1936365','mode':'0o700','physical_type':'UNRESOLVED_OLD_DISPATCHER_OTHER'},
                               'global_holder_absence_proven':False,'maintenance_authority':False}}
    (destination/'deployment-manifest.INACTIVE.json').write_bytes(encoded(manifest))
    chunks=['import sys,types\n']
    for name in ('stale_socket_binding_readonly','home_socket_safety_readonly'):
        path=SOURCE/(name+'.py')
        chunks.extend([f'm=types.ModuleType({name!r});m.__file__={str(path)!r};sys.modules[m.__name__]=m\n',
                       f'exec(compile({path.read_text()!r},{name!r},"exec"),m.__dict__)\n'])
    chunks.append("sys.modules['home_socket_safety_readonly'].main()\n")
    reader=destination/'socket-safety-reader.pinned.py';reader.write_text(''.join(chunks))
    code=f"""import hashlib,os,stat
p={str(reader)!r}
expected={sha(reader.read_bytes())!r}
fd=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
try:
 parts=p.strip('/').split('/')
 for part in parts[:-1]:
  child=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd);os.close(fd);fd=child
 f=os.open(parts[-1],os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC,dir_fd=fd)
 try:
  s=os.fstat(f)
  if not stat.S_ISREG(s.st_mode) or s.st_nlink!=1 or not 0<s.st_size<=2097152:raise SystemExit('BLOCKED reader metadata')
  raw=os.pread(f,s.st_size+1,0)
  if len(raw)!=s.st_size or hashlib.sha256(raw).hexdigest()!=expected:raise SystemExit('BLOCKED reader hash')
 finally:os.close(f)
finally:os.close(fd)
exec(compile(raw,'pinned-socket-safety','exec'),{{'__name__':'__main__'}})
"""
    # Fish single quotes escape only literal apostrophes and backslashes.
    quoted="'"+code.replace('\\','\\\\').replace("'","\\'")+"'"
    fish='#!/usr/bin/fish\n# Metadata/procfs only; no service, GPG, FD borrowing or mutation.\nif not test -t 0\n echo "BLOCKED: local terminal required"\n exit 1\nend\n/usr/bin/sudo /usr/bin/python3 -I -S -B -c '+quoted+'\nexit $status\n'
    (destination/'check-socket-safety.fish').write_text(fish)
    return manifest

if __name__=='__main__':prepare();print(CANDIDATE)

"""Audit-first inventory of one failed/repeated inert transaction. No recovery writes.

Uses definitions embedded from the exact historical bundle, never its main/apply.
"""
import json
import os
from pathlib import Path
import stat

# BOUND and tx are injected by the immutable standalone reader builder.

def error(stage,e):return {'stage':stage,'errno':e.errno if isinstance(e,OSError) else None,'kind':type(e).__name__}

def inspect(path,spec=None):
    parent=None;pin=None
    try:
        parent=tx.walk(str(Path(path).parent))
        try:s=os.stat(Path(path).name,dir_fd=parent,follow_symlinks=False)
        except FileNotFoundError:return {'path':path,'state':'ABSENT'}
        pin=os.open(Path(path).name,os.O_PATH|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=parent)
        tx.require(tx.fp(s)==tx.fp(os.fstat(pin)),'inventory inode pin race')
        anchor='/proc/self/fd/'+str(parent)+'/'+Path(path).name
        acl={n:os.getxattr(anchor,n,follow_symlinks=False).hex() for n in sorted(tx.ACL.intersection(os.listxattr(anchor,follow_symlinks=False)))}
        kind='regular' if stat.S_ISREG(s.st_mode) else 'directory' if stat.S_ISDIR(s.st_mode) else 'socket' if stat.S_ISSOCK(s.st_mode) else 'symlink' if stat.S_ISLNK(s.st_mode) else 'other'
        item={'path':path,'state':'EXISTS','type':kind,'metadata':tx.metadata(s),'acl':acl,
              'transaction_inode_provenance':'NO_HISTORICAL_PER_OBJECT_INODE_LEDGER'}
        if spec:
            item['manifest_metadata_match']=(kind==spec['type'] and (s.st_uid,s.st_gid,stat.S_IMODE(s.st_mode))==(spec['uid'],spec['gid'],int(spec['mode'],8)) and not acl and (kind=='directory' or s.st_nlink==spec.get('nlink',1)))
            if kind=='regular' and item['manifest_metadata_match']:
                record,_=tx.public_file(path,spec['uid'],spec['gid'],int(spec['mode'],8))
                item['sha256']=record['sha256'];item['manifest_hash_match']=record['sha256']==spec['sha256']
        tx.require(tx.fp(s)==tx.fp(os.fstat(pin))==tx.fp(os.stat(Path(path).name,dir_fd=parent,follow_symlinks=False)),'inventory post-read race')
        tx.reachable(str(Path(path).parent),parent)
        return item
    except (OSError,RuntimeError) as e:return {'path':path,'state':'BLOCKED_INVENTORY',**error('object-inventory',e)}
    finally:
        if pin is not None:os.close(pin)
        if parent is not None:os.close(parent)

def names(path):
    fd=tx.walk(path)
    try:
        info=os.fstat(fd)
        journal=tx.STATE+'/deployment/'+BOUND['envelope']['transaction_id']
        if path in (tx.STATE+'/deployment',journal):
            tx.require((info.st_uid,info.st_gid,stat.S_IMODE(info.st_mode))==(0,0,0o700),'exact private root audit directory')
        s=tx.fp(info);result=sorted(os.listdir(fd))
        tx.require(s==tx.fp(os.fstat(fd)),'directory list changed');tx.reachable(path,fd)
        return result
    finally:os.close(fd)

def read_audit(path):
    record,raw=tx.public_file(path,0,0,0o600)
    return {'metadata':record,'data':json.loads(raw)}

def audit_checks(records):
    env=BOUND['envelope'];manifest=BOUND['manifest'];checks={}
    def data(name):return records.get(name,{}).get('data')
    claim=data('claim.json')
    checks['claim_bound_to_original']=claim=={'transaction_id':env['transaction_id'],'manifest_sha256':BOUND['manifest_sha256'],
                  'bundle_sha256':BOUND['bundle_sha256'],'capture_sha256':tx.sha(tx.canonical(env))}
    checks['manifest_exact_bytes']=records.get('manifest.json',{}).get('metadata',{}).get('sha256')==BOUND['manifest_sha256']
    checks['manifest_structure_matches']=data('manifest.json')==manifest
    checks['preflight_matches_original_capture']=data('preflight.json')==env
    post=data('postflight.json')
    checks['successful_postflight_bound']=bool(isinstance(post,dict) and post.get('status')=='PASS_INERT_FILE_DEPLOYMENT' and
             post.get('manifest_sha256')==BOUND['manifest_sha256'] and
             post.get('protected_capture_sha256')==tx.sha(tx.canonical(env['capture']['preservation'])) and
             post.get('agent_started') is False and post.get('activation_approved') is False and post.get('daemon_reloaded') is False)
    checks['rollback_absent']='rollback.json' not in records
    return checks

def recommend(checks,objects,preservation_match,manager_ok):
    full=all(o.get('state')=='EXISTS' and o.get('manifest_metadata_match') is True and
             (o.get('type')!='regular' or o.get('manifest_hash_match') is True) for o in objects)
    if all(checks.values()) and full and preservation_match and manager_ok:
        return 'VERIFY_EXISTING_INERT_INSTALLATION_RETAIN_ALL_NO_REPLAY'
    return 'BLOCKED_UNPROVEN_OR_INCOMPLETE_KEEP_ALL_REQUIRE_SEPARATE_RECOVERY_PLAN'

def review():
    context=tx.host_context();env=BOUND['envelope'];txid=env['transaction_id']
    tx.require(txid=='57652e06b38ac2bd6d153ce5d217b3d9','exact historical transaction')
    journal=tx.STATE+'/deployment/'+txid
    errors=[];records={};audit_names=[]
    # Audit FIRST. No source/policy/target inventory precedes it.
    try:
        audit_names=names(journal)
        for name in ('claim.json','manifest.json','preflight.json','postflight.json','rollback.json'):
            if name in audit_names:
                try:records[name]=read_audit(journal+'/'+name)
                except (OSError,RuntimeError,ValueError) as e:errors.append(error('audit:'+name,e))
    except (OSError,RuntimeError) as e:errors.append(error('audit-directory',e))
    checks=audit_checks(records)
    manifest=BOUND['manifest']
    specs=manifest['directories']+manifest['files']
    objects=[inspect(s['target'],s) for s in specs]
    journal_entries=[inspect(journal+'/'+name) for name in audit_names]
    extra={}
    for path,prefix in ((str(Path(tx.RELEASE).parent),'.home-route-v2-20261103.'),
                        (str(Path(tx.UNIT).parent),'.'+tx.UNIT_NAME+'.'),
                        (journal,'.agent-lifecycle-v2.'),(tx.ROLLBACK,''),(tx.STATE+'/deployment','')):
        try:extra[path]=[inspect(path+'/'+n) for n in names(path) if n.startswith(prefix)]
        except (OSError,RuntimeError) as e:extra[path]={'error':error('staging-or-sibling-inventory',e)}
    for name in audit_names:
        if name.startswith('.agent-lifecycle-v2.'):
            try:extra[journal+'/'+name]=[inspect(journal+'/'+name+'/'+n) for n in names(journal+'/'+name)]
            except (OSError,RuntimeError) as e:errors.append(error('actor-stage-contents',e))
    # Enumerate every publication container, including incomplete staged members.
    member_specs={Path(s['target']).name:s for s in manifest['files']}
    for path in (tx.RELEASE,tx.ACTOR,tx.STATE,tx.STATE+'/quarantine'):
        try:
            children=names(path)
            tx.require(len(children)<=512,'bounded container inventory')
            extra[path]=[inspect(path+'/'+n,dict(member_specs[n],target=path+'/'+n) if n in member_specs and path in (tx.RELEASE,tx.ACTOR) else None) for n in children]
            if path==tx.RELEASE and set(children)!={'agent_lifecycle_949.py','native_socket_route_readonly.py','policy.json'}:
                errors.append({'stage':'release-member-set','kind':'EXTRA_OR_MISSING'})
            if path==tx.ACTOR and set(children)!={'lifecycle.lock'}:
                errors.append({'stage':'actor-member-set','kind':'EXTRA_OR_MISSING'})
            if path==tx.STATE+'/quarantine' and children:
                errors.append({'stage':'quarantine-contents','kind':'UNEXPECTED_NOT_AUTHORIZED_HERE'})
        except (OSError,RuntimeError) as e:errors.append(error('publication-container-inventory',e))
    for item in list(extra.get(str(Path(tx.RELEASE).parent),[])):
        if not isinstance(item,dict) or item.get('type')!='directory':continue
        path=item['path']
        try:
            extra[path]=[inspect(path+'/'+n,dict(member_specs[n],target=path+'/'+n) if n in member_specs else None) for n in names(path)]
        except (OSError,RuntimeError) as e:errors.append(error('release-staging-members',e))
    protected=None;preserved=False;manager=None;manager_ok=False
    try:
        protected=tx.preservation(manifest);preserved=protected==env['capture']['preservation']
    except (OSError,RuntimeError) as e:errors.append(error('protected-preservation',e))
    try:manager=tx.systemd_collisions(allow_published=True);manager_ok=True
    except (OSError,RuntimeError) as e:errors.append(error('systemd-readonly-state',e))
    after=[inspect(s['target'],s) for s in specs]
    stable=objects==after and context==tx.host_context()
    try:stable=stable and audit_names==names(journal)
    except (OSError,RuntimeError) as e:errors.append(error('audit-directory-recheck',e));stable=False
    if not stable:errors.append({'stage':'post-inventory-stability','kind':'IDENTITY_OR_METADATA_CHANGED'})
    # Re-read known evidence; errors/changes cannot become provenance.
    for name,record in records.items():
        try:
            if read_audit(journal+'/'+name)!=record:errors.append({'stage':'audit-stability','kind':'CHANGED'})
        except (OSError,RuntimeError,ValueError) as e:errors.append(error('audit-recheck:'+name,e))
    recommendation=recommend(checks,objects,preserved,manager_ok) if not errors else 'BLOCKED_INCOMPLETE_READONLY_EVIDENCE_RETAIN_ALL'
    return {'status':'READONLY_PARTIAL_DEPLOYMENT_INVENTORY_CAPTURED','transaction_id':txid,
            'manifest_sha256':BOUND['manifest_sha256'],'original_bundle_sha256':BOUND['bundle_sha256'],
            'audit_read_first':True,'journal':journal,'audit_records':records,'audit_checks':checks,'journal_entries':journal_entries,
            'objects':objects,'staging_and_sibling_entries':extra,'protected_preservation_matches_original':preserved,
            'protected_current':protected,'systemd_state':manager,'stability_pass':stable,'errors':errors,
            'last_collision_invocation':'INITIAL_PREFLIGHT_BEFORE_FIRST_WRITE',
            'current_inode_transaction_ownership_proven':False,
            'inode_provenance_limit':'old audit records bind hashes but retain no historical per-created-object inode ledger',
            'recovery_decision':recommendation,'old_transaction_reusable':False,
            'host_changed':False,'agent_started':False,'activation_approved':False,'quarantine_authorized':False,'signing_attempted':False}

def main():
    try:result=review()
    except Exception as e:result={'status':'BLOCKED_READONLY_PARTIAL_DEPLOYMENT_INVENTORY',**error('root-context-or-protocol',e),
                       'host_changed':False,'activation_approved':False,'quarantine_authorized':False}
    print(json.dumps(result,sort_keys=True))

if __name__=='__main__':main()

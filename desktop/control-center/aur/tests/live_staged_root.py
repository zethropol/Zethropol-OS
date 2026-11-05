"""Real unprivileged detached-mount test; never installs or signs anything.

Optional --root/--tree stages a historical unsigned rehearsal; it does NOT
certify root ownership, manifest authorization or independent rebuild evidence.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from staged_root import StagedRoot
from sealed import sandbox_arguments
from worker import tree, harden, canonical, digest
from provision import manifest_tree
from quota import QuotaDirectory


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path);parser.add_argument('--tree',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if bool(args.root)!=bool(args.tree): parser.error('--root and --tree must be paired')
    args.output.mkdir(mode=0o700,parents=True,exist_ok=False)
    aur=Path(__file__).resolve().parents[1]
    helper=(args.output/'root-stage-helper').resolve()
    subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',str(aur/'root_stage.c'),'-o',str(helper)],check=True)
    quotahelper=(args.output/'quota-helper').resolve()
    subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',f'-DROOT_STAGE_PATH="{helper}"',str(aur/'quota_mount.c'),'-o',str(quotahelper)],check=True)
    thawprobe=(args.output/'thaw-probe').resolve()
    subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',str(aur/'tests/root_thaw_probe.c'),'-o',str(thawprobe)],check=True)
    with tempfile.TemporaryDirectory(prefix='aur-staged-probe-') as temporary:
        base=Path(temporary);root=base/'root';(root/'usr/bin').mkdir(parents=True)
        subprocess.run(['/usr/bin/cc','-static','-O2','-Wall','-Wextra','-Werror',str(aur/'tests/sandbox_probe.c'),'-o',str(root/'usr/bin/probe')],check=True)
        (root/'alias').symlink_to('/usr/bin/probe')
        work=base/'work';work.mkdir();sentinel=base/'secret';sentinel.write_text('unchanged')
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen()
        victim=subprocess.Popen(['/usr/bin/sleep','30'])
        fd=os.open(work,os.O_RDONLY|os.O_DIRECTORY)
        try:
            with StagedRoot(root,manifest_tree(root),helper,limit=8*1024**2,max_entries=128) as staged:
                thaw=subprocess.run([str(thawprobe),str(staged.mountfd)],pass_fds=staged.fds,capture_output=True,text=True,timeout=10)
                (args.output/'thaw-probe.log').write_text(thaw.stdout+thaw.stderr)
                assert thaw.returncode==0,(thaw.returncode,thaw.stderr)
                (root/'usr/bin/probe').write_bytes(b'changed source after freeze')
                try: os.open('escape',os.O_WRONLY|os.O_CREAT,0o600,dir_fd=staged.fd)
                except OSError as error: assert error.errno==errno.EROFS,error
                else: raise AssertionError('Parent can mutate published root')
                with QuotaDirectory(quotahelper,base/'quota',byte_limit=1024**2,inodes=128) as quota:
                    command=staged.command(quota.fd,['/usr/bin/probe',str(sentinel),str(victim.pid),str(listener.getsockname()[1])])
                    argv=[str(quotahelper),'--enter',*(str(n) for n in quota.namespaces),'--',*command]
                    result=subprocess.run(argv,pass_fds=(*staged.fds,quota.fd,*quota.namespaces),preexec_fn=harden,env={'PATH':'/usr/bin'},capture_output=True,text=True,timeout=20)
                    assert (quota.path/'allowed').exists(),result.stderr
                (args.output/'probe.log').write_text(result.stdout+result.stderr)
                print(result.stdout+result.stderr,end='',flush=True)
                assert result.returncode==0,result.returncode
            assert sentinel.read_text()=='unchanged' and victim.poll() is None
        finally:
            os.close(fd);listener.close();victim.terminate();victim.wait()
    report={'production_authorized':False,'independent_rebuild':False,'signed_manifest_verified':False,
            'writable_clone_and_superblock_reconfigure':'rejected',
            'small_root_probe':'passed','parent_write_after_freeze':'EROFS','source_mutation_after_freeze':'isolated',
            'helper_sha256':digest(helper.read_bytes()),'full_root':None}
    if args.root:
        entries=json.loads(args.tree.read_bytes());started=time.monotonic()
        baseline=len(os.listdir('/proc/self/fd'))
        with StagedRoot(args.root,entries,helper,limit=2*1024**3,max_entries=65536) as staged:
            with tempfile.TemporaryDirectory(prefix='aur-full-root-work-') as temporary:
                workfd=os.open(temporary,os.O_RDONLY|os.O_DIRECTORY)
                try:
                    command=staged.command(workfd,['/usr/bin/true'])
                    for _ in range(2):
                        with sandbox_arguments(command,(*staged.fds,workfd)) as (argv,fds):
                            result=subprocess.run(argv,pass_fds=fds,preexec_fn=harden,env={'PATH':'/usr/bin'},capture_output=True,text=True,timeout=20)
                        assert result.returncode==0,(result.returncode,result.stderr)
                    (args.output/'full-root-probe.log').write_text(result.stdout+result.stderr)
                    assert result.returncode==0,(result.returncode,result.stderr)
                finally: os.close(workfd)
            report['full_root']={'scope':'historical unsigned user-owned rehearsal only','tree_sha256':digest(canonical(entries)),
                'entries':len(entries),'staged_entries':staged.entry_count,'staged_tree_sha256':staged.tree_sha256,
                'allocated_tmpfs_bytes':staged.allocated_bytes,'logical_bytes':staged.logical_bytes,'root_transfer_fds':len(staged.fds),
                'command_arguments':len(command),'mount_operations':1,'seconds':time.monotonic()-started,
                'true_returncode':result.returncode,'successful_repeated_launches':2,'parent_extra_open_fds':len(os.listdir('/proc/self/fd'))-baseline}
        assert len(os.listdir('/proc/self/fd'))==baseline,'root FD cleanup leaked'
        with StagedRoot(args.root,entries,helper,limit=2*1024**3,max_entries=65536) as repeated:
            assert repeated.tree_sha256==report['full_root']['staged_tree_sha256']
            report['full_root']['independent_preparations_same_user']=2
            report['full_root']['repeated_staged_tree_sha256']=repeated.tree_sha256
        assert len(os.listdir('/proc/self/fd'))==baseline,'repeat root FD cleanup leaked'
    # Failing copy and a pre-cancelled operation must release all setup handles.
    with tempfile.TemporaryDirectory(prefix='aur-staged-failure-') as temporary:
        source=Path(temporary);(source/'bad').write_bytes(b'actual')
        wrong={'.':['dir',0o755],'bad':['file',0o444,'0'*64]}
        before=len(os.listdir('/proc/self/fd'))
        try: StagedRoot(source,wrong,helper,limit=1024**2,max_entries=128)
        except Exception as error: assert 'changed' in str(error),error
        else: raise AssertionError('Wrong hash accepted')
        assert len(os.listdir('/proc/self/fd'))==before,'failure leaked mount/socket/process FDs'
        report['failed_copy_cleanup']='no FD leak'
    (args.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__': main()

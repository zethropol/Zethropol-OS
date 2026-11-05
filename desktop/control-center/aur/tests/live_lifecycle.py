"""Real user-cgroup cancellation/timeout/SIGKILL recovery, harmless payload only."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from journal import Journal
from scoped import ScopedWorker, manager_env
from worker import Rejected


def wait_ready(path, child):
    deadline = time.monotonic()+10
    while not path.exists():
        if child.poll() is not None: raise RuntimeError('Supervisor exited before ready')
        if time.monotonic()>deadline: raise RuntimeError('Readiness timeout')
        time.sleep(0.02)
    return json.loads(path.read_text())


def payload(ready):
    descendant = subprocess.Popen(['/usr/bin/sleep','30'],start_new_session=True)
    Path(ready).write_text(json.dumps({'pid':os.getpid(),'descendant':descendant.pid}))
    time.sleep(30)


def child(store, operation, mode):
    journal = Journal(store)
    directory = Path(store)/operation
    worker = ScopedWorker(store); worker.journal = journal
    with journal.lease(operation):
        try:
            worker.execute([sys.executable, '-B', str(Path(__file__).resolve()), '--payload', str(directory/'ready')], directory,
                lambda: mode=='cancel' and (directory/'ready').exists(), 1 if mode=='timeout' else 20)
        except Rejected as error:
            expected = 'Cancelled' if mode=='cancel' else 'timed out'
            if mode!='crash' and expected not in str(error): raise
            journal.finish(operation, {'state':'cancelled' if mode=='cancel' else 'failed','reason':str(error)})
        else:
            raise RuntimeError('Sleeping fixture unexpectedly succeeded')
    journal.close()


def main():
    with tempfile.TemporaryDirectory(prefix='zethropol-aur-lifecycle-') as temporary:
        store = Path(temporary)/'store'; store.mkdir(mode=0o700)
        journal = Journal(store)
        try:
            for mode in ('cancel','timeout','crash'):
                operation = journal.create('a'*64,30)
                directory = store/operation; directory.mkdir(mode=0o700)
                supervisor = subprocess.Popen([sys.executable,'-B',str(Path(__file__).resolve()),'--child',str(store),operation,mode])
                try:
                    pids = wait_ready(directory/'ready',supervisor)
                    if mode=='crash':
                        row = journal.status(operation)
                        unit = json.loads(row['result'])['scopes'][-1]
                        info = subprocess.run(['/usr/bin/systemctl','--user','show',unit,
                            '--property=TasksMax','--property=MemoryMax','--property=MemorySwapMax','--property=CPUQuotaPerSecUSec'],
                            env=manager_env(),capture_output=True,text=True,check=True)
                        assert 'TasksMax=128' in info.stdout,info.stdout
                        assert 'MemoryMax=2147483648' in info.stdout,info.stdout
                        assert 'MemorySwapMax=0' in info.stdout,info.stdout
                        print(info.stdout,end='')
                        assert journal.recover()==[]
                        supervisor.kill(); supervisor.wait(timeout=5)
                        assert journal.recover()==[operation]
                        assert journal.status(operation)['state']=='interrupted'
                    else:
                        assert supervisor.wait(timeout=10)==0
                        expected = 'cancelled' if mode=='cancel' else 'failed'
                        assert journal.status(operation)['state']==expected
                    # Check the descendant's actual process state: a new session
                    # must not escape cgroup cleanup. Zombies execute no code.
                    for pid in pids.values():
                        deadline=time.monotonic()+3
                        while Path(f'/proc/{pid}/stat').exists():
                            if Path(f'/proc/{pid}/stat').read_text().split(') ',1)[1].startswith('Z'): break
                            if time.monotonic()>deadline: raise RuntimeError('Live descendant escaped cleanup')
                            time.sleep(0.02)
                    print('PASS real cgroup lifecycle:',mode,flush=True)
                finally:
                    if supervisor.poll() is None: supervisor.kill(); supervisor.wait()
            assert journal.recover()==[]
        finally:
            journal.close()

if __name__=='__main__':
    if len(sys.argv)>1 and sys.argv[1]=='--payload': payload(sys.argv[2])
    elif len(sys.argv)>1 and sys.argv[1]=='--child': child(*sys.argv[2:])
    else: main()

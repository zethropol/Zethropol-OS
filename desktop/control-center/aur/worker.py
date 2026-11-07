"""Experimental offline AUR worker. No privileged entry point or installer.

Only a trusted, immutable build root and a sealed review plan may be used.
This module is not connected to the UI or installed by CMake.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import selectors
import signal
import stat
import subprocess
import time
import urllib.request
import uuid
from urllib.parse import urlsplit


class Rejected(RuntimeError):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


def regular(path, limit=256 * 1024 * 1024):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > limit:
            raise Rejected('Not a bounded regular file')
        data = stream.read(limit + 1)
        if len(data) > limit:
            raise Rejected('File size limit')
        return data


def trusted_entry(info):
    # Symlink permission bits are always 0777 on Linux; ownership still matters.
    if info.st_uid != 0 or (not stat.S_ISLNK(info.st_mode) and info.st_mode & 0o022):
        raise Rejected('Build root must be root-owned and immutable to users')


def tree(path, trusted=False):
    """Bind every recipe/local source/build-root file, never follow symlinks."""
    root = Path(path)
    if root.is_symlink() or not root.is_dir():
        raise Rejected('Invalid tree root')
    result = {}
    for item in [root, *sorted(root.rglob('*'))]:
        info = item.lstat()
        if trusted:
            trusted_entry(info)
        name = str(item.relative_to(root))
        if stat.S_ISLNK(info.st_mode):
            if not trusted:
                raise Rejected('Recipe symlink rejected')
            result[name] = ['link', os.readlink(item)]
        elif stat.S_ISDIR(info.st_mode):
            result[name] = ['dir', stat.S_IMODE(info.st_mode)]
        elif stat.S_ISREG(info.st_mode):
            result[name] = ['file', stat.S_IMODE(info.st_mode), digest(regular(item))]
        else:
            raise Rejected('Special files are not permitted')
    return result


def root_binding(entries):
    """Compact commitment to the complete tree, never a client-selected path.

    A new source-plan review is required. Every consumer recomputes this from
    the provisioned root; it is not an ownership or signature substitute.
    """
    if not isinstance(entries, dict) or not 1 <= len(entries) <= 65536:
        raise Rejected('Bounded root tree required')
    data = canonical(entries)
    if len(data) > 16 * 1024**2:
        raise Rejected('Root tree commitment byte quota exceeded')
    return {'sha256': digest(data), 'entries': len(entries)}


def matches_root(plan, entries):
    schema = plan.get('schema')
    if type(schema) is not int or schema not in (1, 2):
        raise Rejected('Unsupported reviewed source-plan version')
    if schema == 1:
        return plan.get('rootfs') == entries
    binding = plan.get('rootfs')
    if (not isinstance(binding, dict) or set(binding) != {'sha256', 'entries'} or
            type(binding['entries']) is not int or not 1 <= binding['entries'] <= 65536 or
            not isinstance(binding['sha256'], str) or
            not re.fullmatch(r'[0-9a-f]{64}', binding['sha256'])):
        raise Rejected('Exact reviewed root commitment required')
    return binding == root_binding(entries)


def review(recipe, rootfs, identity, sources, dependencies, schema=1):
    """Called after declaration review. Input sources have name/url/sha256.

    Deliberately supports only SHA256 HTTPS and local files; no VCS, SKIP,
    redirects, keys or dynamically evaluated source arrays.
    """
    if type(schema) is not int or schema not in (1, 2):
        raise Rejected('Unsupported reviewed source-plan version')
    files = tree(recipe)
    if not all(key in files for key in ('PKGBUILD', '.SRCINFO')):
        raise Rejected('Missing recipe')
    if not re.fullmatch(r'[0-9a-f]{40}', identity.get('commit', '')):
        raise Rejected('Missing pinned Git identity')
    epoch=identity.get('source_date_epoch')
    if epoch is not None and (type(epoch) is not int or not 0<epoch<=int(time.time())):
        raise Rejected('Invalid approved source date epoch')
    seen = set()
    for source in sources:
        name = source['name']
        if not re.fullmatch(r'[A-Za-z0-9_+.-]+', name) or name in ('.', '..') or name in seen or name in ('PKGBUILD', '.SRCINFO'):
            raise Rejected('Ambiguous source filename')
        seen.add(name)
        if not re.fullmatch(r'[0-9a-f]{64}', source['sha256']):
            raise Rejected('Strong source checksum required')
        url = urlsplit(source['url'])
        if source['url'] and (url.scheme != 'https' or not url.hostname or url.username or url.password or url.fragment):
            raise Rejected('Only explicit HTTPS sources supported')
        if not source['url'] and files.get(name, [None])[0] != 'file':
            raise Rejected('Missing local source')
    root_entries = tree(rootfs, trusted=True)
    plan = {'schema': schema, 'identity': identity, 'recipe': files,
            'rootfs': root_entries if schema == 1 else root_binding(root_entries), 'sources': sources,
            'dependencies': sorted(dependencies)}
    return json.loads(canonical(plan)), digest(canonical(plan))


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        raise Rejected('Source redirect requires a new reviewed URL')


def download(source, destination, cancel=lambda: False):
    """Trusted downloader only. No recipe commands, proxy environment or redirects."""
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    with opener.open(source['url'], timeout=30) as response:
        chunks=[]; size=0
        while True:
            if cancel(): raise Rejected('Cancelled')
            block=response.read1(65536)
            if not block: break
            size+=len(block)
            if size>256*1024*1024: raise Rejected('Source checksum/size mismatch')
            chunks.append(block)
        data=b''.join(chunks)
    if len(data) > 256 * 1024 * 1024 or digest(data) != source['sha256']:
        raise Rejected('Source checksum/size mismatch')
    with open(destination, 'xb') as stream:
        stream.write(data)


def harden():
    # Apply before exec: inherited by bwrap, makepkg and all recipe children.
    if ctypes.CDLL(None, use_errno=True).prctl(38, 1, 0, 0, 0) != 0:
        os._exit(126)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (2**30, 2**30))
    resource.setrlimit(resource.RLIMIT_NOFILE, (256, 256))
    resource.setrlimit(resource.RLIMIT_AS, (2**31, 2**31))
    resource.setrlimit(resource.RLIMIT_CPU, (900, 900))
    # Per-UID RLIMIT_NPROC also counts the desktop session, so it is not a
    # worker process quota. Production requires delegated cgroup pids.max.


class Worker:
    def __init__(self, store):
        self.store = Path(store)

    def freeze_root(self, root, entries, limit, max_entries, cancel):
        from sealed import FrozenTree
        return FrozenTree(root, entries, limit=limit, max_entries=max_entries)

    def create_workspace(self, logdir):
        work = logdir / 'work'
        work.mkdir(mode=0o700)
        return work

    def open_workspace(self, work):
        return os.open(work, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)

    def archive_result(self, archive, name, work, logdir):
        return {'path': str(work / 'out' / name), 'sha256': archive.sha256}

    def release_workspace(self):
        pass

    def run(self, plan, approved_hash, recipe, rootfs, cancel=lambda: False, timeout=1800, operation_id=None):
        if plan.get('kind') == 'normal-makepkg':
            return self.run_normal(plan, approved_hash, recipe, cancel, timeout, operation_id)
        operation = operation_id or str(uuid.uuid4())
        if str(uuid.UUID(operation)) != operation:
            raise Rejected("Invalid operation ID")
        logdir = self.store / operation
        logdir.mkdir(mode=0o700, parents=False)
        (logdir / 'worker.log').touch(mode=0o600)
        result = {'operation': operation, 'source': 'aur', 'state': 'failed',
                  'plan_sha256': approved_hash, 'archives': []}
        def checkpoint():
            if cancel():
                raise Rejected('Cancelled')
        try:
            if os.getuid() == 0 or os.geteuid() == 0:
                raise Rejected('Worker must never run as root')
            if digest(canonical(plan)) != approved_hash:
                raise Rejected('Reviewed plan changed')
            rebuilt, _ = review(recipe, rootfs, plan['identity'], plan['sources'], plan['dependencies'], schema=plan.get('schema'))
            if rebuilt != plan:
                raise Rejected('Reviewed inputs changed')
            root_entries_tree = tree(rootfs, True)
            if tree(recipe) != plan['recipe'] or not matches_root(plan, root_entries_tree):
                raise Rejected('Recipe/build root changed since preview')
            checkpoint()
            getattr(self, "report", lambda stage: None)("staging")
            work = self.create_workspace(logdir)
            for name, entry in plan['recipe'].items():
                if entry[0] == 'dir':
                    if name != '.':
                        (work / name).mkdir(parents=True, exist_ok=True)
                else:
                    target = work / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    data = regular(Path(recipe) / name)
                    if digest(data) != entry[2]:
                        raise Rejected('Recipe changed while staging')
                    target.write_bytes(data)
            getattr(self, 'report', lambda stage: None)('verifying-sources')
            for source in plan['sources']:
                checkpoint()
                target = work / source['name']
                if source['url']:
                    download(source, target, cancel)
                if digest(regular(target)) != source['sha256']:
                    raise Rejected('Source checksum mismatch')
            from sealed import Blob, beneath
            workfd = self.open_workspace(work)
            try:
                from root_limits import root_limits
                _, _, root_bytes, root_entries = root_limits(getattr(self, 'root_schema', 1))
                with self.freeze_root(rootfs, root_entries_tree, root_bytes, root_entries, cancel) as frozen:
                    self._sandbox_fds = (*frozen.fds, workfd)
                    commands = ([['/usr/bin/pacman', '-T', '--', *plan['dependencies']]] if plan['dependencies'] else [])
                    commands += [
                        ['/usr/bin/makepkg', '--noconfirm', '--holdver', '--verifysource'],
                        ['/usr/bin/makepkg', '--noconfirm', '--holdver', '--cleanbuild']]
                    for command in commands:
                        checkpoint()
                        stage = 'dependencies' if command[0].endswith('pacman') else ('verifysource' if '--verifysource' in command else 'building')
                        getattr(self, 'report', lambda stage: None)(stage)
                        self.execute(frozen.command(workfd, command, plan['identity'].get('source_date_epoch')), logdir, cancel, timeout)
                    metadata={}
                    if getattr(self,'capture_metadata',False):
                        getattr(self,'report',lambda stage:None)('verifying-package-metadata')
                        out=beneath(workfd,'out',os.O_RDONLY|os.O_DIRECTORY)
                        try:
                            for name in sorted(os.listdir(out)):
                                if not re.fullmatch(r'[A-Za-z0-9@._+:-]+\.pkg\.tar(?:\.[A-Za-z0-9]+)?',name): raise Rejected('Unexpected metadata output filename')
                                checkpoint();offset=(logdir/'worker.log').stat().st_size
                                self.execute(frozen.command(workfd,['/usr/bin/bsdtar','-xOf','/build/out/'+name,'.PKGINFO']),logdir,cancel,min(timeout,15))
                                with open(logdir/'worker.log','rb') as stream:
                                    stream.seek(offset);data=stream.read(1024**2+1)
                                if len(data)>1024**2: raise Rejected('Package metadata quota exceeded')
                                fields={}
                                for line in data.decode('utf-8',errors='strict').splitlines():
                                    key,separator,value=line.partition(' = ')
                                    if separator and key in ('pkgname','pkgbase','pkgver','arch'):
                                        if key in fields: raise Rejected('Duplicate package metadata identity')
                                        fields[key]=value
                                if set(fields)!={'pkgname','pkgbase','pkgver','arch'}: raise Rejected('Incomplete native package metadata')
                                metadata[name]=fields
                        finally: os.close(out)

                checkpoint()
                getattr(self, 'report', lambda stage: None)('exporting')
                outfd = beneath(workfd, 'out', os.O_RDONLY | os.O_DIRECTORY)
                try:
                    for name in sorted(os.listdir(outfd)):
                        if not re.fullmatch(r'[A-Za-z0-9@._+:-]+\.pkg\.tar(?:\.[A-Za-z0-9]+)?', name):
                            raise Rejected('Unexpected build output')
                        sourcefd = beneath(outfd, name)
                        try:
                            with Blob(sourcefd) as archive:
                                entry=self.archive_result(archive,name,work,logdir)
                                if getattr(self,'capture_metadata',False): entry['metadata']=metadata[name]
                                result['archives'].append(entry)
                        finally:
                            os.close(sourcefd)
                finally:
                    os.close(outfd)
            finally:
                self._sandbox_fds = ()
                os.close(workfd)
            if not result['archives']:
                raise Rejected('No package archives produced')
            result['state'] = 'built-untrusted'
            result['reason'] = 'Build only; independent attestation and receipt-backed broker authorization required'
        except (OSError, ValueError, Rejected, subprocess.SubprocessError) as error:
            result['reason'] = str(error)
            with open(logdir / 'worker.log', 'a') as log:
                log.write('\nWorker rejected: ' + str(error) + '\n')
            if str(error) == 'Cancelled':
                result['state'] = 'cancelled'
        finally:
            try:
                temporary = logdir / 'result.pending'
                with open(temporary, 'xb') as stream:
                    stream.write(canonical(result))
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(temporary, logdir / 'result.json')
                dirfd = os.open(logdir, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(dirfd)
                finally:
                    os.close(dirfd)
            finally:
                self.release_workspace()
        return result

    def run_normal(self, plan, approved_hash, recipe, cancel, timeout, operation_id):
        """Reuse quota/scopes/journal with the host's read-only makepkg tools."""
        import tarfile
        operation = operation_id or str(uuid.uuid4())
        if str(uuid.UUID(operation)) != operation or os.getuid() == 0 or os.geteuid() == 0:
            raise Rejected('Canonical operation and non-root desktop user required')
        encoded = json.dumps(plan, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()
        if digest(encoded) != approved_hash: raise Rejected('Reviewed build plan changed')
        review = plan['review']
        if not re.fullmatch('[0-9a-f]{40}', review['commit']): raise Rejected('Pinned Git commit required')
        logdir = self.store / operation; logdir.mkdir(mode=0o700)
        (logdir/'worker.log').touch(mode=0o600)
        result = {'operation':operation,'source':'aur','state':'failed','plan_sha256':approved_hash,'archives':[]}
        proc = None
        try:
            work = self.create_workspace(logdir)
            proc = subprocess.Popen(['/usr/bin/git','-c','core.hooksPath=/dev/null','-c','core.attributesFile=/dev/null','-C',str(recipe),'archive','--format=tar',review['commit']],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,env={'PATH':'/usr/bin','LC_ALL':'C','GIT_CONFIG_NOSYSTEM':'1','GIT_CONFIG_GLOBAL':'/dev/null','GIT_CONFIG_SYSTEM':'/dev/null'})
            total = 0
            with tarfile.open(fileobj=proc.stdout,mode='r|') as archive:
                for member in archive:
                    if cancel(): raise Rejected('Cancelled')
                    path = Path(member.name)
                    if path.is_absolute() or '..' in path.parts or not (member.isdir() or member.isfile()): raise Rejected('Unsafe Git recipe entry')
                    total += member.size
                    if total > 64*1024**2: raise Rejected('Recipe quota exceeded')
                    target = work/path
                    if member.isdir(): target.mkdir(mode=0o700,parents=True,exist_ok=True)
                    else:
                        target.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
                        with open(target,'xb') as output: output.write(archive.extractfile(member).read())
                        target.chmod(0o600)
            if proc.wait(timeout=15): raise Rejected('Pinned Git recipe extraction failed')
            for name,key in [('PKGBUILD','pkgbuild_sha256'),('.SRCINFO','srcinfo_sha256')]:
                if digest(regular(work/name,1024**2)) != review[key]: raise Rejected('Reviewed recipe bytes changed')
            for name in ('out','sources','build'): (work/name).mkdir(mode=0o700)
            workfd = self.open_workspace(work)
            try:
                self._sandbox_fds = (workfd,)
                def command(argv, network=False):
                    args = ['/usr/bin/bwrap','--die-with-parent','--new-session','--unshare-all','--unshare-user','--disable-userns','--assert-userns-disabled','--cap-drop','ALL','--uid',str(os.getuid()),'--gid',str(os.getgid()),'--ro-bind','/usr','/usr','--symlink','usr/bin','/bin','--symlink','usr/bin','/sbin','--symlink','usr/lib','/lib','--symlink','usr/lib','/lib64','--ro-bind','/etc','/etc','--ro-bind','/var/lib/pacman','/var/lib/pacman','--proc','/proc','--dev','/dev','--size','268435456','--tmpfs','/tmp','--dir','/home','--bind-fd',str(workfd),'/build','--chdir','/build','--clearenv','--setenv','PATH','/usr/bin','--setenv','HOME','/tmp','--setenv','LC_ALL','C','--setenv','GNUPGHOME','/tmp/gnupg','--setenv','PKGDEST','/build/out','--setenv','SRCDEST','/build/sources','--setenv','BUILDDIR','/build/build']
                    if network:
                        args += ['--share-net']
                        resolver = os.path.realpath('/etc/resolv.conf')
                        if resolver != '/etc/resolv.conf': args += ['--ro-bind',resolver,resolver]
                    return args + ['--remount-ro','/','--',*argv]
                self.report('checking-pkgbuild-declarations')
                offset = (logdir/'worker.log').stat().st_size
                self.execute(command(['/usr/bin/makepkg','--printsrcinfo']),logdir,cancel,min(timeout,30))
                with open(logdir/'worker.log','rb') as log:
                    log.seek(offset); actual = log.read(1024**2+1)
                if len(actual)>1024**2: raise Rejected('Runtime source metadata quota exceeded')
                def declarations(data):
                    return sorted(line.strip() for line in data.decode('utf-8').splitlines()
                                  if ' = ' in line and not line.strip().startswith('#'))
                if declarations(actual) != declarations(regular(work/'.SRCINFO',1024**2)):
                    raise Rejected('PKGBUILD declarations differ from reviewed .SRCINFO')
                dependencies = review.get('dependencies',[])
                steps = []
                if dependencies: steps.append(('dependencies',['/usr/bin/pacman','-T','--',*dependencies],False))
                steps += [('verifying-sources',['/usr/bin/makepkg','--noconfirm','--holdver','--verifysource'],True),('building',['/usr/bin/makepkg','--noconfirm','--holdver','--cleanbuild','--nosign'],False)]
                for stage,argv,network in steps:
                    if cancel(): raise Rejected('Cancelled')
                    self.report(stage); self.execute(command(argv,network),logdir,cancel,timeout)
                from sealed import Blob
                for path in sorted((work/'out').iterdir()):
                    if not re.fullmatch(r'[A-Za-z0-9@._+:-]+\.pkg\.tar(?:\.[A-Za-z0-9]+)?',path.name): raise Rejected('Unexpected makepkg output')
                    fd = os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
                    try:
                        with Blob(fd) as blob: result['archives'].append(self.archive_result(blob,path.name,work,logdir))
                    finally: os.close(fd)
            finally:
                self._sandbox_fds = (); os.close(workfd)
            if not result['archives']: raise Rejected('makepkg produced no package')
            result.update(state='built-untrusted',reason='Build complete; native package/plan verification and explicit PolicyKit install approval remain required')
        except (OSError,ValueError,KeyError,Rejected,subprocess.SubprocessError,tarfile.TarError) as error:
            result.update(state='cancelled' if str(error)=='Cancelled' else 'failed',reason=str(error))
        finally:
            if proc is not None:
                if proc.poll() is None: proc.kill()
                proc.wait(); proc.stdout.close()
            try: self.release_workspace()
            except Exception as error: result.update(state='interrupted',reason='Quota cleanup failed: '+str(error),archives=[])
            (logdir/'result.json').write_bytes(canonical(result))
        return result

    def execute(self, args, logdir, cancel, timeout):
        # Standalone/single-thread supervisor only: preexec_fn is intentionally
        # never invoked from the multithreaded Qt process.
        from sealed import sandbox_arguments
        with sandbox_arguments(args, getattr(self, '_sandbox_fds', ())) as (spawn_args, spawn_fds), open(logdir / 'worker.log', 'ab', buffering=0) as log:
            proc = subprocess.Popen(spawn_args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    env=getattr(self, '_process_env', {'PATH': '/usr/bin', 'LC_ALL': 'C'}),
                                    start_new_session=True, preexec_fn=harden,
                                    pass_fds=spawn_fds)
            start = time.monotonic()
            selector = selectors.DefaultSelector()
            selector.register(proc.stdout, selectors.EVENT_READ)
            os.set_blocking(proc.stdout.fileno(), False)
            try:
                while selector.get_map() or proc.poll() is None:
                    if cancel():
                        raise Rejected('Cancelled')
                    if time.monotonic() - start > timeout:
                        raise Rejected('Worker timed out')
                    for key, _ in selector.select(0.05):
                        block = os.read(key.fileobj.fileno(), 65536)
                        if not block:
                            selector.unregister(key.fileobj)
                            continue
                        if os.fstat(log.fileno()).st_size + len(block) > 8*1024**2:
                            raise Rejected('Worker log quota exceeded')
                        log.write(block)
                if proc.returncode:
                    raise Rejected(f'Worker failed/crashed ({proc.returncode}); inspect worker.log')
            finally:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
                selector.close()
                proc.stdout.close()



class InstallationBroker:
    """Fail-closed production boundary. Never trusts user-produced manifests.

    A real implementation must copy archives to root-owned storage via held FDs,
    load through libalpm with current signature policy, resolve under its lock,
    authenticate original D-Bus peer, consume a server-owned plan, enforce all
    recovery gates and publish post-commit evidence. None is emulated here.
    """
    def install(self, *args, **kwargs):
        raise Rejected('AUR installation disabled: authenticated broker and recovery integration unavailable')

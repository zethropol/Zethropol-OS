"""Bounded extraction of signed, pinned build-root packages; never scripts."""
from dataclasses import dataclass
import os
from pathlib import Path, PurePosixPath
import stat
import tarfile

from root_limits import root_limits
from trust import approved_manifest
from worker import Rejected, canonical, digest


@dataclass(frozen=True)
class ProvisionedRoot:
    path: Path
    manifest: dict
    receipt: dict
    # An isolated-test signature can NEVER become production-policy provenance.
    scope: str


def manifest_tree(root):
    result = {'.':['dir',0o755]}
    for path in sorted(root.rglob('*')):
        name = str(path.relative_to(root)); info = path.lstat()
        if stat.S_ISLNK(info.st_mode): result[name]=['link',os.readlink(path)]
        elif stat.S_ISDIR(info.st_mode): result[name]=['dir',0o755]
        elif stat.S_ISREG(info.st_mode): result[name]=['file',stat.S_IMODE(info.st_mode),digest(path.read_bytes())]
        else: raise Rejected('Special build-root file')
    return result


def package_database(root, records, max_bytes, max_entries):
    """Script-free local ALPM metadata derived only from signed .PKGINFO.

    pacman -T inside the sealed root must observe the provisioned packages,
    never the host database or worker claims. This performs no transaction.
    """
    local = root / 'var/lib/pacman/local'
    fields = {'pkgname': 'NAME', 'pkgver': 'VERSION', 'pkgbase': 'BASE',
              'pkgdesc': 'DESC', 'url': 'URL', 'arch': 'ARCH',
              'builddate': 'BUILDDATE', 'packager': 'PACKAGER', 'size': 'SIZE',
              'license': 'LICENSE', 'depend': 'DEPENDS', 'optdepend': 'OPTDEPENDS',
              'makedepend': 'MAKEDEPENDS', 'checkdepend': 'CHECKDEPENDS',
              'provides': 'PROVIDES', 'conflict': 'CONFLICTS', 'replaces': 'REPLACES'}
    documents = []
    for record in records:
        sections = []
        for source, target in fields.items():
            values = record.get(source, [])
            if any(not value or '\x00' in value or '\r' in value or '\n' in value or
                   (value.startswith('%') and value.endswith('%')) for value in values):
                raise Rejected('Ambiguous signed package database field')
            if values:
                sections.append('%' + target + '%\n' + '\n'.join(values) + '\n\n')
        documents.append((record['pkgname'][0] + '-' + record['pkgver'][0], ''.join(sections).encode()))
    if 2 + sum(len(data) for _, data in documents) > max_bytes or 5 + 2 * len(documents) > max_entries:
        raise Rejected('Generated package database quota exceeded')
    local.mkdir(parents=True, mode=0o755, exist_ok=False)
    (local / 'ALPM_DB_VERSION').write_bytes(b'9\n')
    for name, data in documents:
        directory = local / name
        directory.mkdir(mode=0o755)
        (directory / 'desc').write_bytes(data)


def provision(destination, signed_manifest, packages, max_bytes=None, max_entries=None):
    plan, plan_hash = approved_manifest(signed_manifest)
    _, archive_limit, expanded_limit, entry_limit = root_limits(plan['schema'])
    if max_bytes is not None and (type(max_bytes) is not int or not 0 < max_bytes <= expanded_limit):
        raise Rejected('Extraction override exceeds signed root byte budget')
    if max_entries is not None and (type(max_entries) is not int or not 0 < max_entries <= entry_limit):
        raise Rejected('Extraction override exceeds signed root entry budget')
    max_bytes = expanded_limit if max_bytes is None else max_bytes
    max_entries = entry_limit if max_entries is None else max_entries
    if len(packages)!=len(plan['packages']): raise Rejected('Incomplete signed package set')
    if sum(package.archive.size for package in packages)>min(archive_limit, max_bytes):
        raise Rejected('Aggregate signed input quota exceeded')
    root = Path(destination)
    root.mkdir(mode=0o700, parents=False, exist_ok=False)
    links = {}; names=set(); used=0; expanded=0; count=0
    receipts=[]; metadata=[]
    try:
        for expected, signed in zip(plan['packages'], packages):
            if signed.scope!=signed_manifest.scope or signed.archive.sha256!=expected['sha256']:
                raise Rejected('Signed package differs from approved root manifest')
            info_fields = None; package_files=set()
            stream = os.fdopen(os.open(f'/proc/self/fd/{signed.archive.fd}',os.O_RDONLY),'rb')
            with stream, tarfile.open(fileobj=stream,mode='r:*') as archive:
                for member in archive:
                    count+=1
                    if count>max_entries: raise Rejected('Build-root inode quota exceeded')
                    # Count ignored metadata too, before tarfile advances over
                    # it. A compressed .MTREE/.BUILDINFO bomb must not evade the
                    # expansion budget merely because it is not extracted.
                    if member.size < 0:
                        raise Rejected('Invalid root archive member size')
                    expanded += member.size
                    if expanded > max_bytes:
                        raise Rejected('Build-root expansion quota exceeded')
                    name = member.name.removeprefix('./').rstrip('/')
                    if name in ('', '.'): continue
                    parts=PurePosixPath(name).parts
                    if len(name)>4096 or str(PurePosixPath(name))!=name or name.startswith('/') or '..' in parts or name=='.provenance.json':
                        raise Rejected('Unsafe build-root archive path')
                    # The real pacman package ships this empty directory.
                    # Only our verified metadata generator may populate it.
                    if name == 'var/lib/pacman' and member.isdir():
                        continue
                    if name == 'var/lib/pacman' or name.startswith('var/lib/pacman/'):
                        raise Rejected('Package archive may not supply the root package database')
                    if member.name.removeprefix('./')=='.PKGINFO':
                        if info_fields is not None or not member.isfile() or member.size>1024**2: raise Rejected('Ambiguous package metadata')
                        data=archive.extractfile(member).read()
                        info_fields={}
                        for line in data.decode().splitlines():
                            if ' = ' in line:
                                key,value=line.split(' = ',1)
                                info_fields.setdefault(key,[]).append(value)
                        continue
                    if parts[0] in ('.INSTALL','.MTREE','.BUILDINFO'):
                        if len(parts)!=1 or not member.isfile(): raise Rejected('Invalid package metadata entry')
                        continue
                    if any('/'.join(parts[:i]) in links for i in range(1,len(parts)+1)):
                        raise Rejected('Archive traverses deferred link')
                    target=root/name
                    if member.isdir():
                        if target.exists() and not target.is_dir(): raise Rejected('Root path conflict')
                        target.mkdir(mode=0o755,parents=True,exist_ok=True)
                        continue
                    if name in names or target.exists(): raise Rejected('Duplicate root file/path conflict')
                    names.add(name)
                    if member.issym():
                        # Resolution is confined later by the sealed namespace;
                        # no link is created during extraction.
                        if not member.linkname or len(member.linkname)>4096 or '\x00' in member.linkname: raise Rejected('Invalid root symlink')
                        links[name]=member.linkname
                        continue
                    if member.islnk() and plan['schema']==2:
                        # Materialize only a backward reference to a regular
                        # file from THIS verified package. No filesystem link
                        # is followed or created; aliases consume full logical
                        # bytes because the sealed sandbox copies them too.
                        source_name=member.linkname.removeprefix('./')
                        if source_name not in package_files:
                            raise Rejected('Hardlink must reference an earlier regular file in the same package')
                        from sealed import beneath
                        rootfd=os.open(root,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
                        try: sourcefd=beneath(rootfd,source_name)
                        finally: os.close(rootfd)
                        with os.fdopen(sourcefd,'rb') as source:
                            info=os.fstat(source.fileno())
                            if not stat.S_ISREG(info.st_mode): raise Rejected('Hardlink source must be regular')
                            used+=info.st_size;expanded+=info.st_size
                            if used>max_bytes or expanded>max_bytes: raise Rejected('Hardlink expansion quota exceeded')
                            target.parent.mkdir(mode=0o755,parents=True,exist_ok=True)
                            with open(target,'xb') as output:
                                remaining=info.st_size
                                while remaining:
                                    block=source.read(min(remaining,1024**2))
                                    if not block: raise Rejected('Truncated hardlink source')
                                    output.write(block);remaining-=len(block)
                            target.chmod((info.st_mode&0o555)|0o444)
                        package_files.add(name)
                        continue
                    if not member.isfile(): raise Rejected('Unsupported hardlink/device/special archive entry')
                    used+=member.size
                    if member.size<0 or used>max_bytes: raise Rejected('Build-root byte quota exceeded')
                    target.parent.mkdir(mode=0o755,parents=True,exist_ok=True)
                    with archive.extractfile(member) as source, open(target,'xb') as output:
                        remaining=member.size
                        while remaining:
                            block=source.read(min(remaining,1024**2))
                            if not block: raise Rejected('Truncated root archive')
                            output.write(block); remaining-=len(block)
                    # Version 2 is a public verification root, not a host
                    # installation. Signed repository bytes must be readable
                    # by all three isolated builders; no write/setid bits survive.
                    target.chmod((member.mode & 0o555) | (0o444 if plan['schema']==2 else 0))
                    package_files.add(name)
            if info_fields is None or info_fields.get('pkgname')!=[expected['name']] or info_fields.get('pkgver')!=[expected['version']] or info_fields.get('arch') not in ([plan['architecture']],['any']):
                raise Rejected('Signed root package name/version/architecture mismatch')
            receipts.append({'name':expected['name'],'sha256':signed.archive.sha256,'signer':signed.signer})
            metadata.append(info_fields)
        # Reserve the database path before deferred symlinks can be published.
        # Otherwise a package-supplied var/lib symlink could redirect writes.
        for name in links:
            if 'var/lib/pacman'.startswith(name + '/'):
                raise Rejected('Root link shadows package database')
        package_database(root, metadata, max_bytes-used, max_entries-count)
        for name, link in links.items():
            if any(file.startswith(name+'/') for file in names if file!=name):
                raise Rejected('Root link shadows archive content')
            target=root/name
            if target.exists(): raise Rejected('Root symlink replaces directory')
            target.parent.mkdir(mode=0o755,parents=True,exist_ok=True)
            target.symlink_to(link)
        receipt={'manifest_sha256':plan_hash,'scope':signed_manifest.scope,'packages':receipts}
        (root/'.provenance.json').write_bytes(canonical(receipt))
        (root/'.provenance.json').chmod(0o444)
        root.chmod(0o755)  # Root-owned, desktop-readable; never user-writable.
        return ProvisionedRoot(root,manifest_tree(root),receipt,signed_manifest.scope)
    except BaseException:
        # Caller-owned staging is deliberately left unapproved, not reused.
        # No receipt is emitted on failure; caller may clean its temporary tree.
        raise

def provision_production(manifest_fd, signature_fd, package_fds):
    """Provider-side FD API; no recipe execution, caller paths or signing.

    Intended for the trusted provisioning publisher, never the desktop worker.
    All trust is loaded from fixed root-owned public policies. Packaging and
    invocation by a trusted provider are external deployment requirements.
    """
    from contextlib import ExitStack
    from trust import SignedArchive, TrustPolicy
    if os.getuid()!=0 or os.geteuid()!=0:
        raise Rejected('Production provisioning requires trusted root provider')
    if not isinstance(package_fds,list) or not 1<=len(package_fds)<=256:
        raise Rejected('Bounded signed distribution closure required')
    parent=Path('/var/lib/zethropol/aur-build-roots')
    for path in [parent,*parent.parents]:
        m=path.lstat()
        if not stat.S_ISDIR(m.st_mode) or m.st_uid!=0 or m.st_mode&0o022:
            raise Rejected('Unsafe provisioning parent')
    with ExitStack() as stack:
        manifest_policy=TrustPolicy.load('/etc/zethropol-update/aur-trust/policy.json',role='manifest')
        stack.callback(manifest_policy.keyring.close)
        distribution_policy=TrustPolicy.load('/etc/zethropol-update/aur-trust/distribution.json')
        stack.callback(distribution_policy.keyring.close)
        manifest=stack.enter_context(SignedArchive(manifest_fd,signature_fd,manifest_policy))
        plan, _ = approved_manifest(manifest)
        if len(package_fds) != len(plan['packages']):
            raise Rejected('Signed root package count mismatch')
        _, archive_limit, _, _ = root_limits(plan['schema'])
        packages=[];total=0
        for archive, signature in package_fds:
            verified=stack.enter_context(SignedArchive(archive,signature,distribution_policy,
                                                       archive_limit=archive_limit-total))
            total+=verified.archive.size;packages.append(verified)
        return provision(parent/manifest.archive.sha256,manifest,packages)

"""Create a deterministic file:// Go proxy from upstream go.sum-pinned bytes.

No network, compilation, key operations or installation. Module zip/go.mod h1
hashes are independently recomputed using Go's dirhash.Hash1 format. The result
is an additional reviewed source artifact, never an attestation.
"""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tarfile
import zipfile


def hash1(files):
    lines = []
    for name, data in sorted(files):
        if '\n' in name or '\r' in name:
            raise ValueError('Invalid module hash filename')
        lines.append(hashlib.sha256(data).hexdigest() + '  ' + name + '\n')
    return 'h1:' + base64.b64encode(hashlib.sha256(''.join(lines).encode()).digest()).decode()


def ziphash(data, prefix):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        files = []; names = set(); total = 0
        items = archive.infolist()
        if len(items) > 16384:
            raise ValueError('Module ZIP entry quota exceeded')
        for item in items:
            name = item.filename
            mode = item.external_attr >> 16
            normalized = name[:-1] if item.is_dir() else name
            if (name in names or not name.startswith(prefix + '/') or
                    str(PurePosixPath(normalized)) != normalized or
                    '..' in PurePosixPath(name).parts or '\\' in name or
                    '\x00' in name or '\n' in name or '\r' in name or
                    (stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)) or
                    (stat.S_ISDIR(mode) and not item.is_dir()) or
                    item.file_size > 64 * 1024**2):
                raise ValueError('Unsafe module ZIP')
            names.add(name)
            if item.is_dir():
                if item.file_size:
                    raise ValueError('Unsafe module ZIP directory payload')
                continue
            total += item.file_size
            if total > 512 * 1024**2 or len(names) > 16384:
                raise ValueError('Module ZIP expansion quota exceeded')
            files.append((name, archive.read(item)))
        return hash1(files)


def unescape(value):
    result = ''; index = 0
    while index < len(value):
        if value[index] == '!':
            index += 1
            if index == len(value) or not 'a' <= value[index] <= 'z':
                raise ValueError('Invalid Go proxy escape')
            result += value[index].upper()
        else:
            result += value[index]
        index += 1
    return result


def proxy_files(root):
    """Bounded, no-follow traversal; even a symlinked directory is rejected.

    Read through held directory FDs so replacing an ancestor cannot redirect a
    checksum input. Cache contents never supply authentication on their own.
    """
    rootfd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        entries = 0
        for directory, dirs, files, directoryfd in os.fwalk('.', dir_fd=rootfd, follow_symlinks=False):
            entries += len(dirs) + len(files)
            if entries > 16384:
                raise ValueError('Proxy traversal entry quota exceeded')
            for name in dirs:
                if stat.S_ISLNK(os.stat(name, dir_fd=directoryfd, follow_symlinks=False).st_mode):
                    raise ValueError('Proxy directory must not be a symlink')
            for name in files:
                if Path(name).suffix not in ('.zip', '.mod', '.info'):
                    continue
                relative = (PurePosixPath(directory) / name).as_posix()
                if '/@v/' not in relative or relative.startswith('sumdb/'):
                    continue
                if not stat.S_ISREG(os.stat(name, dir_fd=directoryfd, follow_symlinks=False).st_mode):
                    raise ValueError('Proxy artifact must be a bounded regular file')
                fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directoryfd)
                with os.fdopen(fd, 'rb') as stream:
                    info = os.fstat(stream.fileno())
                    if not stat.S_ISREG(info.st_mode) or info.st_size > 64 * 1024**2:
                        raise ValueError('Proxy artifact must be a bounded regular file')
                    data = stream.read(64 * 1024**2 + 1)
                    if len(data) > 64 * 1024**2:
                        raise ValueError('Proxy artifact too large')
                yield relative, data
    finally:
        os.close(rootfd)


def bundle(cache, sums, output):
    pinned = {}
    for line in Path(sums).read_text().splitlines():
        module, version, checksum = line.split()
        key = (module, version)
        if key in pinned and pinned[key] != checksum:
            raise ValueError('Conflicting go.sum entry')
        pinned[key] = checksum
    payload = []; records = []; size = 0
    for relative, data in proxy_files(cache):
        suffix = PurePosixPath(relative).suffix
        module, leaf = relative.split('/@v/')
        module = unescape(module); version = unescape(leaf.rsplit('.', 1)[0])
        if (module, version) not in pinned and (module, version + '/go.mod') not in pinned:
            raise ValueError('Module/version absent from original go.sum: ' + relative)
        size += len(data)
        if size > 256 * 1024**2 or len(records) >= 4096:
            raise ValueError('Offline proxy quota exceeded')
        if suffix == '.zip':
            actual = ziphash(data, module + '@' + version)
            if actual != pinned.get((module, version)):
                raise ValueError('Module ZIP h1 mismatch: ' + relative)
        elif suffix == '.mod':
            actual = hash1([('go.mod', data)])
            if actual != pinned.get((module, version + '/go.mod')):
                raise ValueError('Module go.mod h1 mismatch: ' + relative)
        else:
            if json.loads(data).get('Version') != version:
                raise ValueError('Module info version mismatch')
            actual = None  # .info is pinned by the bundle SHA, not by go.sum.
        payload.append((relative, data))
        records.append({'path': relative, 'sha256': hashlib.sha256(data).hexdigest(), 'go_sum_h1': actual})
    if not any(p['path'].endswith('.zip') for p in records):
        raise ValueError('No pinned module archives')
    target = Path(output)
    with target.open('xb') as stream, tarfile.open(fileobj=stream, mode='w', format=tarfile.USTAR_FORMAT) as archive:
        for name, data in sorted(payload):
            item = tarfile.TarInfo('go-proxy/' + name)
            item.size = len(data); item.mode = 0o444; item.mtime = 0
            archive.addfile(item, io.BytesIO(data))
    return {'go_sum_sha256': hashlib.sha256(Path(sums).read_bytes()).hexdigest(),
            'bundle_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
            'bytes': target.stat().st_size, 'files': sorted(records, key=lambda item: item['path']),
            'production_attestation': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', required=True)
    parser.add_argument('--go-sum', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    report = bundle(args.cache, args.go_sum, args.output)
    Path(args.output + '.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('bundle_sha256', 'bytes')}))

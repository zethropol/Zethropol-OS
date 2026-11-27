"""User-session coordination for managed CPU models; no service or privilege changes.

This advisory lease is not an authorization boundary against a malicious host
process with the same UID. It does not control the preserved legacy 8087 model.
"""
import fcntl
import json
import os
from pathlib import Path
import re
import stat

GIB = 1024**3


class BusyError(RuntimeError):
    pass


class ResourceError(RuntimeError):
    pass


def available_ram():
    try:
        return int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines()
                        if line.startswith('MemAvailable:'))) * 1024
    except (OSError, StopIteration, ValueError, IndexError) as error:
        raise ResourceError('Kullanılabilir RAM doğrulanamadı; model başlatılmadı.') from error


def _process_identity(pid):
    """Public proc metadata only; never read process environments or arguments."""
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        status = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines()
                      if ':' in line)
        if fields[0] in {'Z', 'X'}:
            return None
        return {'pid': pid, 'start_time': fields[19], 'parent_pid': int(fields[1]),
                'uid': int(status['Uid'].split()[0])}
    except (FileNotFoundError, ProcessLookupError):
        return None
    except (OSError, ValueError, IndexError, KeyError) as error:
        raise ResourceError('Model süreç kimliği doğrulanamadı.') from error


def _private_directory(fd):
    info = os.fstat(fd)
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ResourceError('Model kilidi için kullanıcıya özel bir çalışma dizini gerekir.')


class ModelLease:
    """Acquire before loading a model; release only after the owned child stops.

    runtime_dir is an explicit trusted test seam. Production callers should use
    the fixed /run/user/<uid> default and must not derive it from model input.
    """
    def __init__(self, owner, *, runtime_dir=None, min_available_bytes=12*GIB):
        if not isinstance(owner, str) or not re.fullmatch(r'[a-z0-9][a-z0-9_.-]{0,63}', owner):
            raise ValueError('Invalid managed model owner')
        if type(min_available_bytes) is not int or min_available_bytes <= 0:
            raise ValueError('A positive RAM headroom requirement is mandatory')
        self.owner = owner
        self.runtime_dir = Path(runtime_dir) if runtime_dir is not None else Path('/run/user') / str(os.getuid())
        self.min_available_bytes = min_available_bytes
        self._fd = None
        self._pid = None
        self._record = None

    def _open(self):
        flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
        base = child = None
        try:
            base = os.open(self.runtime_dir, flags)
            _private_directory(base)
            try:
                os.mkdir('zethropol-inference', mode=0o700, dir_fd=base)
            except FileExistsError:
                pass
            child = os.open('zethropol-inference', flags, dir_fd=base)
            _private_directory(child)
            fd = os.open('cpu.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC,
                         0o600, dir_fd=child)
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1
                    or info.st_uid != os.getuid() or info.st_mode & 0o077):
                os.close(fd)
                raise ResourceError('Model kilit dosyasının sahipliği veya türü güvenli değil.')
            return fd
        except OSError as error:
            raise ResourceError('Güvenli model kilidi açılamadı; model başlatılmadı.') from error
        finally:
            for fd in (child, base):
                if fd is not None:
                    os.close(fd)

    def _write(self, record):
        raw = json.dumps(record, separators=(',', ':')).encode()
        os.lseek(self._fd, 0, os.SEEK_SET)
        os.ftruncate(self._fd, 0)
        if os.write(self._fd, raw) != len(raw):
            raise ResourceError('Model kilit kaydı tamamlanamadı.')
        os.fsync(self._fd)

    @staticmethod
    def _live_child(record):
        child = record.get('child')
        if child is None:
            return False
        if (not isinstance(child, dict) or set(child) != {'pid', 'start_time', 'parent_pid', 'uid'}
                or type(child['pid']) is not int or child['pid'] <= 1
                or type(child['start_time']) is not str or not child['start_time'].isdigit()
                or type(child['parent_pid']) is not int or child['uid'] != os.getuid()):
            raise ResourceError('Önceki model süreç kaydı geçersiz; otomatik başlatma reddedildi.')
        current = _process_identity(child['pid'])
        return bool(current and current['uid'] == child['uid'] and current['start_time'] == child['start_time'])

    def acquire(self):
        if self._fd is not None:
            raise RuntimeError('Model lease is already held')
        fd = self._open()
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise BusyError('Başka bir ZCI/ZEKANS CPU modeli çalışıyor; önce onu normal yoldan kapatın.') from error
            os.lseek(fd, 0, os.SEEK_SET)
            raw = os.read(fd, 4097)
            if raw:
                try:
                    previous = json.loads(raw)
                    if (len(raw) > 4096 or not isinstance(previous, dict)
                            or set(previous) != {'schema', 'owner', 'parent', 'child'}
                            or previous['schema'] != 'zethropol-cpu-lease-v1'):
                        raise ValueError('Bad lease record')
                except (ValueError, TypeError) as error:
                    raise ResourceError('Önceki model kilit kaydı okunamadı; model başlatılmadı.') from error
                if self._live_child(previous):
                    raise BusyError('Önceki başlatıcıdan kalan model hâlâ çalışıyor; ikinci model başlatılmadı.')
            if available_ram() < self.min_available_bytes:
                raise ResourceError('Model başlatılmadı: güvenli kullanılabilir RAM payı yetersiz.')
            self._fd = fd
            self._pid = os.getpid()
            self._record = {'schema': 'zethropol-cpu-lease-v1', 'owner': self.owner,
                            'parent': _process_identity(self._pid), 'child': None}
            self._write(self._record)
            return self
        except BaseException:
            self._fd = None
            os.close(fd)
            raise

    def attach_process(self, pid):
        """Bind the direct owned model child immediately after Popen."""
        if self._fd is None or self._pid != os.getpid() or self._record['child'] is not None:
            raise RuntimeError('A held lease may track exactly one direct child')
        if type(pid) is not int or pid <= 1:
            raise ValueError('Invalid child PID')
        child = _process_identity(pid)
        if child is None or child['uid'] != os.getuid() or child['parent_pid'] != self._pid:
            raise ResourceError('Yalnız bu başlatıcının canlı model süreci izlenebilir.')
        self._record['child'] = child
        self._write(self._record)

    def release(self):
        if self._fd is None:
            return
        fd, self._fd = self._fd, None
        try:
            if self._pid != os.getpid():
                return  # A forked child must not unlock the parent's open-file description.
            if self._live_child(self._record):
                # Preserve metadata: a new launcher must reject an orphan even after this FD closes.
                raise ResourceError('Model hâlâ çalışıyor; kilit kaydı korunuyor. Önce sahip olunan modeli kapatın.')
            os.ftruncate(fd, 0)
            os.fsync(fd)
            fcntl.flock(fd, fcntl.LOCK_UN)
        finally:
            os.close(fd)

    def __enter__(self):
        return self.acquire()

    def __exit__(self, *args):
        self.release()

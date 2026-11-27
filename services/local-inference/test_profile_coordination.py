"""Exercise both real launcher entry paths without loading either model."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
import inference_guard as guard


def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    loaded=importlib.util.module_from_spec(spec)
    sys.modules[name]=loaded
    spec.loader.exec_module(loaded)
    return loaded


class CoordinationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.chat=module('coordinated_chat',HERE.parent/'zci-chat/zci_chat.py')
        cls.code=module('coordinated_code',HERE.parent/'zekans-code/zekans_code.py')

    def test_both_launchers_refuse_before_hashing_or_spawning_while_another_lease_is_held(self):
        with tempfile.TemporaryDirectory(prefix='profile-coordination-') as directory:
            root=Path(directory)
            fake=root/'local-file';fake.write_text('fixture, never executed')
            config=self.chat.Config(engine=fake,model=fake,system_file=fake,model_sha256='0'*64,
                                    profile='gemma4-e4b',engine_port=19087,gateway_port=19086)
            def lease(owner):
                return guard.ModelLease(owner,runtime_dir=root,min_available_bytes=1)
            with lease('other-managed-model'):
                with patch.object(self.chat,'ModelLease',side_effect=lease), \
                     patch.object(self.chat,'file_hash',side_effect=AssertionError('Model read before busy check')) as hashing, \
                     patch.object(self.chat.subprocess,'Popen',side_effect=AssertionError('Second model spawned')) as spawn:
                    with self.assertRaises(self.chat.ChatError):
                        self.chat.Engine(config,root).__enter__()
                    hashing.assert_not_called();spawn.assert_not_called()
                with patch.object(self.code,'ModelLease',side_effect=lease), \
                     patch.object(self.code,'file_hash',side_effect=AssertionError('Model read before busy check')) as hashing, \
                     patch.object(self.code.subprocess,'Popen',side_effect=AssertionError('Second model spawned')) as spawn:
                    with self.assertRaises(guard.BusyError):
                        self.code.Engine({'model':str(fake)},root).__enter__()
                    hashing.assert_not_called();spawn.assert_not_called()
                # A refused launcher must not release the original owner's lock.
                with self.assertRaises(guard.BusyError):lease('third-managed-model').acquire()
            with lease('next-managed-model'):
                pass

    def test_sequential_cross_profile_release_and_ram_guard(self):
        with tempfile.TemporaryDirectory(prefix='profile-coordination-seq-') as directory:
            root = Path(directory)
            def lease(owner, min_ram=1):
                return guard.ModelLease(owner, runtime_dir=root, min_available_bytes=min_ram)

            # 1. Chat holds lease -> Code cannot acquire
            with lease('zci-chat-gemma4-e4b'):
                with self.assertRaises(guard.BusyError):
                    lease('zekans-qwen3-4b').acquire()
            # 2. Chat released -> Code can acquire
            with lease('zekans-qwen3-4b'):
                with self.assertRaises(guard.BusyError):
                    lease('zci-chat-gemma4-e4b').acquire()
            # 3. Memory exhaustion: refuses model start when available RAM < threshold
            with patch.object(guard, 'available_ram', return_value=5 * 1024**3):
                with self.assertRaises(guard.ResourceError) as ctx:
                    lease('zci-chat-gemma4-e4b', min_ram=12 * 1024**3).acquire()
                self.assertIn('kullanılabilir RAM payı yetersiz', str(ctx.exception))
                with self.assertRaises(guard.ResourceError) as ctx:
                    lease('zekans-qwen3-4b', min_ram=12 * 1024**3).acquire()
                self.assertIn('kullanılabilir RAM payı yetersiz', str(ctx.exception))


if __name__=='__main__':unittest.main()

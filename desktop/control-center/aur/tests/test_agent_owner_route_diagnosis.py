import sys
from pathlib import Path
from unittest.mock import patch
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import agent_lifecycle_949 as life

class Diagnosis(unittest.TestCase):
    def probe(self,existing):
        def lookup(path):
            if str(path) in existing:return object()
            raise FileNotFoundError()
        with patch.object(Path,'lstat',autospec=True,side_effect=lookup):
            return life.owner_configuration_presence()
    def test_each_guard_reports_only_fixed_label(self):
        cases={'owner-agent-config':life.GNUPG+'/gpg-agent.conf','owner-common-config':life.GNUPG+'/common.conf','system-route-control':'/etc/gnupg/gpgconf.ctl','vendor-route-control':'/usr/lib/gnupg/gpgconf.ctl','binary-route-control':'/usr/bin/gpgconf.ctl','alternate-runtime-root':'/run/gnupg/user/949'}
        for label,path in cases.items():
            with self.subTest(label=label):self.assertEqual(self.probe({path}),[label])
    def test_no_content_or_symlink_resolution(self):
        with patch.object(Path,'read_text',side_effect=AssertionError()),patch.object(Path,'read_bytes',side_effect=AssertionError()),patch.object(Path,'resolve',side_effect=AssertionError()):
            self.assertEqual(self.probe(set()),[])
    def test_guard_retained_and_io_not_disguised_as_absent(self):
        with patch.object(life,'owner_configuration_presence',return_value=['owner-common-config']):
            with self.assertRaises(life.Refused) as caught:life.owner_configs_absent()
            self.assertEqual(caught.exception.public_blockers,['owner-common-config'])
        with patch.object(Path,'lstat',side_effect=PermissionError(13,'sensitive detail')):
            with self.assertRaises(PermissionError):life.owner_configuration_presence()

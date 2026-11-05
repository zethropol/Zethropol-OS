"""Synthetic readonly planning; no real runtime directory, service or agent."""
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from contextlib import ExitStack,redirect_stdout

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import agent_lifecycle_plan_check as check


class Planning(unittest.TestCase):
    def exercise(self,visible=True):
        old=check.life.GNUPG+'/S.gpg-agent'
        meta={'device':'1','inode':'2','uid':949,'gid':948,'mode':'0o600','is_socket':True,
              'is_symlink':False,'access_acl':False,'default_acl':False}
        def lstat(p):
            if str(p)==old:return SimpleNamespace(st_ctime_ns=1793532307947935714)
            raise FileNotFoundError()
        def text(p):return 'systemd' if str(p)=='/proc/1/comm' else '0 0 4294967295'
        with ExitStack() as stack:
            for name,value in [('getuid',0),('geteuid',0),('getgrouplist',[948])]:
                stack.enter_context(patch.object(check.os,name,return_value=value))
            stack.enter_context(patch.object(check.sys,'flags',SimpleNamespace(isolated=True,no_site=True)))
            stack.enter_context(patch.object(check.sys,'dont_write_bytecode',True))
            stack.enter_context(patch.object(check.pwd,'getpwnam',return_value=SimpleNamespace(pw_name='zethropol-aur-release',pw_uid=949,pw_gid=948,pw_dir=check.life.HOME,pw_shell='/usr/bin/nologin')))
            stack.enter_context(patch.object(check.socket_meta,'config_binding'))
            stack.enter_context(patch.object(check.socket_meta,'metadata',return_value=meta))
            stack.enter_context(patch.object(check.socket_meta,'listener_inventory',return_value={'visibility_complete':visible,'holders':[],'uid949_gpg_agent_processes':0,'matched_socket_entries':[]}))
            stack.enter_context(patch.object(check.life,'node',return_value=SimpleNamespace(st_dev=1,st_ino=2)))
            stack.enter_context(patch.object(check.life,'owner_configs_absent'))
            stack.enter_context(patch.object(check.os,'listdir',return_value=[]))
            stack.enter_context(patch.object(check.Path,'lstat',autospec=True,side_effect=lstat))
            stack.enter_context(patch.object(check.Path,'read_text',autospec=True,side_effect=text))
            stack.enter_context(patch.object(check.Path,'is_file',return_value=True))
            stack.enter_context(patch.object(check,'digest',return_value='a'*64))
            out=io.StringIO()
            with redirect_stdout(out):check.main('b'*64)
            return json.loads(out.getvalue())

    def test_read_only_plan_never_grants_activation_and_preserves_stat_precision(self):
        result=self.exercise()
        self.assertFalse(result['agent_started']);self.assertFalse(result['signing_attempted'])
        self.assertFalse(result['policy_draft_not_authorization']['activation_approved'])
        self.assertEqual(result['policy_draft_not_authorization']['old_socket']['ctime_ns'],'1793532307947935714')

    def test_incomplete_listener_visibility_blocks_planning(self):
        with self.assertRaises(check.life.Refused):self.exercise(visible=False)


if __name__=='__main__':unittest.main()

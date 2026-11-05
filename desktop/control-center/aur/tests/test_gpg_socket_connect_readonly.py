"""Injected syscall stages/kernel metadata only; no real agent or host changes."""
import errno
import json
from pathlib import Path
import struct
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import gpg_socket_connect_readonly as diag


class Socket:
    def __init__(self,fail=None,uid=949,greeting=b'OK SECRET_BANNER\n'):
        self.fail=fail;self.uid=uid;self.greeting=greeting;self.closed=False;self.connected=False
    def settimeout(self,value):pass
    def connect(self,path):
        if self.fail=='connect':raise ConnectionRefusedError(errno.ECONNREFUSED,'SECRET_PATH')
        if self.fail=='permission':raise PermissionError(errno.EACCES,'SECRET_PATH')
        self.connected=True
    def getsockopt(self,*args):
        if self.fail=='peer':raise OSError(errno.ENOPROTOOPT,'SECRET')
        return struct.pack('3i',12345,self.uid,948)
    def recv(self,count):
        if self.fail=='greeting':raise TimeoutError('SECRET')
        return self.greeting
    def close(self):self.closed=True


class ConnectStages(unittest.TestCase):
    def test_creation_eperm_is_distinct_and_policy_facts_are_conditional(self):
        def denied(*args):raise PermissionError(errno.EPERM,'SECRET')
        with patch.object(diag,'policy_context',return_value={'denial_source_proven':False}) as policy:
            result=diag.connect_stages('/public/socket',factory=denied)
        self.assertEqual(result['failed_stage'],'socket-create');self.assertEqual(result['errno_name'],'EPERM')
        self.assertEqual(result['socket_create'],'BLOCKED');self.assertEqual(result['connect'],'NOT_RUN')
        policy.assert_called_once();self.assertNotIn('SECRET',json.dumps(result))

    def test_stale_refusal_keeps_precise_connect_errno(self):
        sock=Socket('connect')
        with patch.object(diag,'policy_context',side_effect=AssertionError('No policy inspection')):
            result=diag.connect_stages('/public/socket',factory=lambda *a:sock)
        self.assertEqual(result['failed_stage'],'unix-connect');self.assertEqual(result['errno_number'],errno.ECONNREFUSED)
        self.assertEqual(result['connect'],'BLOCKED');self.assertEqual(result['peer_credentials'],'NOT_RUN')
        self.assertTrue(sock.closed);self.assertNotIn('SECRET',json.dumps(result))

    def test_eacces_collects_only_related_policy_facts(self):
        with patch.object(diag,'policy_context',return_value={'denial_source_proven':False}) as policy:
            result=diag.connect_stages('/public/socket',factory=lambda *a:Socket('permission'))
        self.assertEqual(result['failed_stage'],'unix-connect');policy.assert_called_once()

    def test_peer_failure_does_not_report_failed_connect(self):
        result=diag.connect_stages('/public/socket',factory=lambda *a:Socket('peer'))
        self.assertEqual(result['connect'],'PASS');self.assertEqual(result['transport'],'CONNECTED')
        self.assertEqual(result['failed_stage'],'so-peercred');self.assertEqual(result['errno_name'],'ENOPROTOOPT')

    def test_greeting_timeout_is_not_invented_as_kernel_connect_errno(self):
        result=diag.connect_stages('/public/socket',factory=lambda *a:Socket('greeting'))
        self.assertEqual(result['failed_stage'],'greeting-read');self.assertEqual(result['connect'],'PASS')
        self.assertIsNone(result['errno_number']);self.assertEqual(result['error_kind'],'timeout')

    def test_foreign_peer_is_not_queried(self):
        sock=Socket(uid=1000);result=diag.connect_stages('/public/socket',factory=lambda *a:sock)
        self.assertEqual(result['result'],'FOREIGN_PEER_UID');self.assertEqual(result['greeting'],'NOT_RUN')
        self.assertFalse(result['command_sent'])

    def test_greeting_never_leaks_and_no_command_is_sent(self):
        result=diag.connect_stages('/public/socket',factory=lambda *a:Socket())
        self.assertEqual(result['greeting'],'OK');self.assertFalse(result['command_sent'])
        self.assertNotIn('SECRET_BANNER',json.dumps(result))

    def test_socket_activation_guard_skips_connect_but_checks_creation(self):
        sock=Socket();result=diag.connect_stages('/public/socket',factory=lambda *a:sock,allow_connect=False)
        self.assertEqual(result['socket_create'],'PASS');self.assertEqual(result['connect'],'SKIPPED_LIFECYCLE_GUARD')
        self.assertFalse(sock.connected);self.assertTrue(sock.closed)

    def test_late_socket_disappearance_does_not_erase_original_connect_error(self):
        before={'device':'1','inode':'2','uid':949,'gid':948,'mode':'0o600'}
        with patch.object(diag.os,'getuid',return_value=949),patch.object(diag.os,'geteuid',return_value=949),\
             patch.object(diag.os,'getgid',return_value=948),patch.object(diag.os,'getegid',return_value=948),\
             patch.object(diag.os,'getgroups',return_value=[948]),patch.object(diag.base,'safe_socket',return_value='safe-existing-socket'),\
             patch.object(diag,'traversal',return_value={'rows':[{'actor_can_traverse':True}],'failure':None}),\
             patch.object(diag,'metadata',side_effect=[before,FileNotFoundError(errno.ENOENT,'SECRET')]),\
             patch.object(diag.os,'access',return_value=True),\
             patch.object(diag,'connect_stages',return_value={'failed_stage':'unix-connect','errno_number':errno.ECONNREFUSED}):
            result=diag.actor('/public/socket',True)
        self.assertEqual(result['errno_number'],errno.ECONNREFUSED)
        self.assertEqual(result['post_observation_error']['errno_number'],errno.ENOENT)

    def test_wrong_homedir_binding_rejected_without_agent_query(self):
        config={'schema':2,'purpose':'zethropol-manifest-signer','identity_model':'existing-key-owner',
                'user':'zethropol-aur-release','uid':949,'gnupg_home':diag.base.HOME,
                'signer':'F3F36BFA6D0FC60DA1CB34F2927519292F6F497B'}
        info=SimpleNamespace(st_mode=0o100644,st_nlink=1,st_uid=0,st_gid=0,st_size=300)
        with patch.object(diag.Path,'lstat',return_value=info),patch.object(diag.Path,'read_bytes',return_value=json.dumps(config).encode()):
            self.assertTrue(diag.config_binding())
        config['gnupg_home']='/other/user/.gnupg'
        with patch.object(diag.Path,'lstat',return_value=info),patch.object(diag.Path,'read_bytes',return_value=json.dumps(config).encode()):
            with self.assertRaises(ValueError):diag.config_binding()


class ListenerEvidence(unittest.TestCase):
    def inventory(self,holders,rows=None,complete=True):
        return {'matched_socket_entries':rows if rows is not None else [{'kernel_inode':'123','listening':True}],
                'holders':holders,'visibility_complete':complete}

    def test_exact_path_kernel_socket_inode_is_not_filesystem_inode(self):
        raw='Num RefCount Protocol Flags Type St Inode Path\n0000: 00000002 00000000 00010000 0001 01 123 /public/socket\n0001: 00000002 00000000 00010000 0001 01 999 /other\n'
        result=diag.unix_rows(raw,'/public/socket')
        self.assertEqual(result,[{'kernel_inode':'123','listening':True,'type':'0001','state':'01'}])

    def test_existing_uid949_agent_listener_allows_read_only_connect(self):
        holder={'kind':'gpg-agent','uid':949,'kernel_inodes':['123']}
        self.assertEqual(diag.decision(self.inventory([holder]))[0],True)

    def test_systemd_only_listener_never_triggers_socket_activation(self):
        holder={'kind':'systemd','uid':949,'kernel_inodes':['123']}
        allow,reason=diag.decision(self.inventory([holder]))
        self.assertFalse(allow);self.assertEqual(reason,'SOCKET_ACTIVATION_POSSIBLE_NO_AGENT_START_ALLOWED')

    def test_even_active_agent_does_not_override_systemd_activation_guard(self):
        holders=[{'kind':'systemd','uid':949,'kernel_inodes':['123']},
                 {'kind':'gpg-agent','uid':949,'kernel_inodes':['123']}]
        self.assertFalse(diag.decision(self.inventory(holders))[0])

    def test_incomplete_visibility_never_assumes_no_activation(self):
        holder={'kind':'gpg-agent','uid':949,'kernel_inodes':['123']}
        self.assertFalse(diag.decision(self.inventory([holder],complete=False))[0])

    def test_foreign_or_unknown_listener_is_not_used(self):
        holder={'kind':'gpg-agent','uid':1000,'kernel_inodes':['123']}
        self.assertFalse(diag.decision(self.inventory([holder]))[0])
        self.assertFalse(diag.decision(self.inventory([]))[0])

    def test_no_listener_and_complete_view_can_probe_stale_endpoint(self):
        self.assertTrue(diag.decision(self.inventory([],[],True))[0])
        self.assertFalse(diag.decision(self.inventory([],[],False))[0])

    def test_errno_unavailable_or_hostile_text_is_never_exposed(self):
        result=diag.failure('unix-connect',OSError('SECRET socket passphrase'))
        self.assertIsNone(result['errno_number']);self.assertNotIn('SECRET',json.dumps(result))


if __name__=='__main__':unittest.main()

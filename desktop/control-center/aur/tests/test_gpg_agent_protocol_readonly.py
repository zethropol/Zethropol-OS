"""Public parser/peer fixtures and dummy client pipes; no real agent or key."""
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import gpg_agent_protocol_readonly as probe


class AssuanParser(unittest.TestCase):
    def test_ok_requires_terminal_status_and_numeric_pid(self):
        result=probe.parse_public_response(b'D 12345\nOK\n')
        self.assertEqual(result['assuan_status'],'OK');self.assertEqual(result['pid'],12345)

    def test_err_is_protocol_error_even_if_cli_exit_is_zero(self):
        result=probe.parse_public_response(b'ERR 67109139 SECRET description\n')
        self.assertEqual(result['assuan_status'],'ERR');self.assertEqual(result['error_code'],67109139)
        self.assertIsNone(result['pid']);self.assertNotIn('SECRET',json.dumps(result))

    def test_fragmented_percent_encoded_data_is_parsed_without_plus_decoding(self):
        result=probe.parse_public_response(b'D 12\nD %33%34%35%0A\nOK\n')
        self.assertEqual(result['pid'],12345)
        self.assertIsNone(probe.parse_public_response(b'D 12+34\nOK\n')['pid'])

    def test_ambiguous_missing_or_injected_records_do_not_produce_pid(self):
        for raw in (b'D 12345\n',b'D 12345\nOK\nOK\n',b'D 12345\nERR 1 SECRET\n',
                    b'D %ZZ\nOK\n',b'D 12%003\nOK\n',b'PRIVATE\nOK\n',b'x'*4097):
            with self.subTest(raw=raw[:20]):self.assertIsNone(probe.parse_public_response(raw)['pid'])

    def test_status_comments_and_crlf_do_not_leak_payloads(self):
        result=probe.parse_public_response(b'# SECRET\r\nS PINENTRY SECRET\r\nD 12345\r\nOK\r\n')
        self.assertEqual(result['pid'],12345);self.assertEqual(result['status_records'],1)
        self.assertNotIn('SECRET',json.dumps(result))


class ExistingAgent(unittest.TestCase):
    def setUp(self):
        for name,value in [('getuid',949),('geteuid',949),('getgid',948),('getegid',948),('getgroups',[948])]:
            p=patch.object(probe.os,name,return_value=value);p.start();self.addCleanup(p.stop)
        p=patch.object(probe.base,'home_metadata',return_value='PASS');p.start();self.addCleanup(p.stop)
        s=SimpleNamespace(st_dev=1,st_ino=2,st_uid=949,st_mode=0o140600)
        p=patch.object(probe.os,'lstat',return_value=s);p.start();self.addCleanup(p.stop)
        self.calls=[]

    def inspect(self,stdout=b'D 12345\nOK\n',rc=0,peer=None,stderr=b''):
        def run(argv):
            self.calls.append(argv)
            if argv[0]=='/usr/bin/gpgconf':return SimpleNamespace(returncode=0,stdout=(probe.base.HOME+'/S.gpg-agent\n').encode(),stderr=b'')
            return SimpleNamespace(returncode=rc,stdout=stdout,stderr=stderr)
        info={'pid':12345,'uid':949,'gid':948,'process':'gpg-agent','process_uid_verified':True,'greeting':'OK'}
        if peer:info.update(peer)
        return probe.inspect(run=run,peer=lambda p:info,socket_check=lambda p:'safe-existing-socket')

    def test_same_user_peer_and_pid_query_pass_without_secret_readiness(self):
        result=self.inspect()
        self.assertEqual(result['agent_access'],'PASS');self.assertTrue(result['pid_matches_peer'])
        self.assertEqual(result['secret_key_presence'],'NOT_PROBED');self.assertEqual(result['passphrase_cache'],'NOT_PROBED')
        self.assertEqual(self.calls[-1][-2:],['GETINFO pid','/bye'])
        self.assertIn('--no-autostart',self.calls[-1]);self.assertIn('--no-history',self.calls[-1])

    def test_cli_zero_assuan_err_is_not_disconnected_agent(self):
        result=self.inspect(b'ERR 67109139 SECRET\n')
        self.assertEqual(result['agent_query_returncode'],0);self.assertEqual(result['agent_access'],'PASS')
        self.assertEqual(result['assuan_status'],'ERR');self.assertEqual(result['result'],'AGENT_ACCESS_PASS_GETINFO_ERR')
        self.assertNotIn('SECRET',json.dumps(result))

    def test_foreign_uid_or_non_agent_peer_stops_before_query(self):
        for fields,code in [({'uid':1000},'PEER_OWNER_BLOCKED'),({'process':'other'},'PEER_NOT_LOCAL_GPG_AGENT')]:
            self.calls=[];result=self.inspect(peer=fields)
            self.assertEqual(result['agent_access'],'BLOCKED');self.assertEqual(result['result'],code)
            self.assertEqual(len(self.calls),1)

    def test_reply_pid_mismatch_is_blocked(self):
        result=self.inspect(b'D 999\nOK\n')
        self.assertEqual(result['agent_access'],'BLOCKED');self.assertEqual(result['result'],'PID_PEER_MISMATCH')

    def test_stderr_only_exposes_counts_not_raw_content(self):
        result=self.inspect(stderr=b'SECRET /private/agent-path')
        self.assertTrue(result['stderr_present']);self.assertNotIn('SECRET',json.dumps(result))

    def test_malformed_output_does_not_turn_into_key_failure(self):
        result=self.inspect(b'D PRIVATE_KEY_OR_PASSWORD\nOK\n')
        self.assertEqual(result['transport'],'PASS');self.assertEqual(result['agent_access'],'PASS')
        self.assertEqual(result['result'],'AGENT_ACCESS_PASS_RESPONSE_UNCLASSIFIED')
        self.assertNotIn('PRIVATE_KEY_OR_PASSWORD',json.dumps(result))


class BoundedClient(unittest.TestCase):
    def test_both_streams_are_bounded_and_drained(self):
        result=probe.collect([sys.executable,'-I','-S','-B','-c',"import os;os.write(1,b'x'*16000);os.write(2,b'y'*12000)"])
        self.assertEqual(result.returncode,0);self.assertEqual(len(result.stdout),4096);self.assertEqual(len(result.stderr),2048)
        self.assertTrue(result.stdout_truncated);self.assertTrue(result.stderr_truncated)

    def test_timeout_only_kills_diagnostic_child(self):
        result=probe.collect([sys.executable,'-I','-S','-B','-c','import time;time.sleep(1)'],timeout=0.03)
        self.assertTrue(result.timed_out);self.assertNotEqual(result.returncode,0)


if __name__=='__main__':unittest.main()

"""Versioned plan commitments, never substitutes for signatures or ownership."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from worker import root_binding, matches_root, canonical, Rejected


class RootCommitment(unittest.TestCase):
    def tree(self):
        return {'.':['dir',0o555], 'usr':['dir',0o555],
                'usr/file':['file',0o444,'a'*64], 'lib':['link','usr/lib']}

    def test_all_tree_properties_remain_bound(self):
        original=self.tree()
        plan={'schema':2,'rootfs':root_binding(original)}
        self.assertTrue(matches_root(plan, original))
        for path, value in [('usr/file',['file',0o444,'b'*64]),
                            ('usr/file',['file',0o555,'a'*64]),
                            ('usr',['dir',0o755]), ('lib',['link','elsewhere'])]:
            changed=copy.deepcopy(original);changed[path]=value
            self.assertFalse(matches_root(plan,changed))
        changed=copy.deepcopy(original);del changed['usr/file']
        self.assertFalse(matches_root(plan,changed))
        changed=copy.deepcopy(original);changed['extra']=['dir',0o555]
        self.assertFalse(matches_root(plan,changed))

    def test_versions_are_explicit_and_legacy_remains_exact(self):
        entries=self.tree()
        self.assertTrue(matches_root({'schema':1,'rootfs':entries},entries))
        self.assertFalse(matches_root({'schema':1,'rootfs':root_binding(entries)},entries))
        for schema in (True,False,None,0,3,'2'):
            with self.assertRaises(Rejected): matches_root({'schema':schema,'rootfs':entries},entries)
        with self.assertRaises(Rejected): matches_root({'schema':2,'rootfs':entries},entries)

    def test_descriptor_cannot_supply_path_or_expanded_limits(self):
        entries=self.tree();binding=root_binding(entries)
        invalid=[dict(binding,path='/tmp/root.json'),dict(binding,entries=True),
                 dict(binding,entries=65537),dict(binding,sha256='A'*64),
                 dict(binding,sha256='a'*63),dict(binding,limit=2**50)]
        for item in invalid:
            with self.assertRaises(Rejected): matches_root({'schema':2,'rootfs':item},entries)

    def test_large_tree_stays_out_of_managed_document(self):
        entries={f'usr/lib/{i:05d}'+('x'*40):['file',0o444,'a'*64] for i in range(60393)}
        self.assertGreater(len(canonical(entries)),1024**2)
        plan={'schema':2,'rootfs':root_binding(entries)}
        self.assertLess(len(canonical(plan)),256)
        self.assertTrue(matches_root(plan,entries))
        with self.assertRaises(Rejected): root_binding({str(i):[] for i in range(65537)})
        with self.assertRaises(Rejected): root_binding({})
        with self.assertRaisesRegex(Rejected,'byte quota'):
            root_binding({'x'*(16*1024**2):['dir',0o555]})


if __name__=='__main__': unittest.main()

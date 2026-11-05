"""Pure precision and acceptance guards; no host, signer or process execution."""
import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from manifest_postflight_checks import encode_snapshot,decode_snapshot,audit_contract


class PostflightChecks(unittest.TestCase):
    def setUp(self):
        self.path='/usr/lib/zethropol/aur/worker.py'
        self.row=(38,1936537,33188,0,0,18264,1793532307970935729,1793532307973935731)
        self.snapshot={self.path:self.row}

    def test_nanoseconds_above_js_safe_integer_preserved_exactly(self):
        encoded=encode_snapshot(self.snapshot)
        roundtrip=json.loads(json.dumps(encoded))
        self.assertEqual(decode_snapshot(roundtrip,{self.path}),self.snapshot)
        self.assertEqual(roundtrip['rows'][self.path][-2],str(self.row[-2]))

    def test_historical_number_rounding_produces_false_drift(self):
        self.assertNotEqual(int(float(self.row[-2])),self.row[-2])
        safe=json.loads(json.dumps(encode_snapshot(self.snapshot)))
        self.assertEqual(int(safe['rows'][self.path][-2]),self.row[-2])

    def test_numeric_bool_float_and_noncanonical_decimal_rejected(self):
        for bad in (self.row[-2],True,float(self.row[-2]),'01','+1','-1','1e18','18446744073709551616'):
            value=encode_snapshot(self.snapshot);value['rows'][self.path][-2]=bad
            with self.assertRaises(ValueError):decode_snapshot(value,{self.path})

    def test_encode_never_accepts_already_rounded_float(self):
        row=list(self.row);row[-2]=float(row[-2])
        with self.assertRaises(ValueError):encode_snapshot({self.path:row})

    def test_missing_extra_paths_and_bad_schema_rejected(self):
        value=encode_snapshot(self.snapshot)
        for expected in (set(),{self.path,'/unexpected'}):
            with self.assertRaises(ValueError):decode_snapshot(value,expected)
        value['schema']=True
        with self.assertRaises(ValueError):decode_snapshot(value,{self.path})

    def test_timestamp_and_inode_checks_are_not_weakened(self):
        restored=decode_snapshot(encode_snapshot(self.snapshot),{self.path})
        for index in (1,6,7):
            changed=list(self.row);changed[index]+=1
            self.assertNotEqual(restored,{self.path:tuple(changed)})

    def records(self):
        rows=[{'event':event,'release':'a'*64} for event in ('preflight-pass','staging-complete','published','success')]
        for row in rows[1:3]:row.update(dev=38,ino=1936581)
        rows[-1].update(files_verified=16,signing=False)
        return rows

    def test_complete_root_audit_binds_release_inode_and_non_signing_success(self):
        self.assertTrue(audit_contract(self.records(),'a'*64,(38,1936581)))

    def test_incomplete_rollback_wrong_release_inode_or_count_cannot_pass(self):
        bad=[self.records()[:-1]]
        for index,key,value in ((3,'event','rollback-quarantined'),(2,'release','b'*64),(2,'ino',1936582),
                                (3,'files_verified',True),(3,'files_verified',15),(3,'signing',True)):
            rows=self.records();rows[index][key]=value;bad.append(rows)
        for rows in bad:
            with self.assertRaises(ValueError):audit_contract(rows,'a'*64,(38,1936581))


if __name__=='__main__':unittest.main()

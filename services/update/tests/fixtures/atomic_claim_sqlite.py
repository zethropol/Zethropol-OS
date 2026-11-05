"""Test-only SQLite adapter. No real authority, root open or freshness guarantee."""
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = Path(__file__).resolve().parent
SCHEMA = (HERE / "atomic_claim_schema.sql").read_text()
STEPS = [s.strip() for s in (HERE / "atomic_claim_transaction.sql").read_text().split(";") if s.strip()]
OP = bytes.fromhex("1096233bf5bb4d2ba9fb1ba5cdc85c5e")
EPOCH = b"e" * 16
PRODUCER = bytes.fromhex("2096233bf5bb4d2ba9fb1ba5cdc85c5e")
CLAIM = bytes.fromhex("3096233bf5bb4d2ba9fb1ba5cdc85c5e")
OTHER = bytes.fromhex("4096233bf5bb4d2ba9fb1ba5cdc85c5e")
APPLICATION_ID = 0x5A434C4D
LEGACY_MARKER = "CREATE TABLE fixture_legacy_marker(version INTEGER NOT NULL CHECK(version=0)) STRICT;"
_EXPECTED_LAYOUTS = {}

def envelope():
    def uid(value):
        import uuid
        return str(uuid.UUID(bytes=value))
    payload = dict(schema_version=1, operation_id=uid(OP),
        commit=dict(result="committed", checkpoint_sha256="a"*64, approved_plan_sha256="b"*64),
        origin_boot_id=uid(PRODUCER), target=dict(kernel_release="synthetic", kernel_sha256="a"*64,
        initramfs_sha256="b"*64, loader_sha256="c"*64, config_sha256="d"*64,
        entry_id=uid(OP), root_filesystem_id=uid(PRODUCER), root_subvolume_id=uid(OP)),
        producer="synthetic", claimed_trust="unverified")
    digest = hashlib.sha256(b"zethropol-post-reboot-envelope-v1\0" + json.dumps(payload,separators=(",", ":")).encode()).hexdigest()
    return dict(payload=payload, binding_sha256=digest)

ENVELOPE = envelope()
BINDING = bytes.fromhex(ENVELOPE["binding_sha256"])

def request(**changes):
    values = dict(epoch=EPOCH, op=OP, gen=1, binding=BINDING, expected_head=2,
        next_head=3, producer=PRODUCER, claim=CLAIM)
    values.update(changes)
    # Fixed, typed byte/decimal fields; this digest is test data, not authority.
    parts = [values[k].hex() if isinstance(values[k],bytes) else str(values[k])
             for k in ("epoch","op","gen","binding","expected_head","producer","claim")]
    values["request_hash"] = hashlib.sha256(json.dumps(parts).encode()).digest()
    return values

def connect(path, timeout=2000):
    # Existing fixture only: missing DB must not be silently recreated.
    from urllib.parse import quote
    db = sqlite3.connect("file:"+quote(str(path))+"?mode=rw", uri=True, isolation_level=None)
    try:
        for setting in ("journal_mode=DELETE", "synchronous=EXTRA", "foreign_keys=ON",
                        "trusted_schema=OFF", "busy_timeout="+str(timeout), "read_uncommitted=OFF"):
            db.execute("PRAGMA "+setting)
        return db
    except Exception:
        db.close()
        raise

def seed(db, missing_create=False):
    db.execute("BEGIN IMMEDIATE")
    db.execute("PRAGMA user_version=1")
    db.execute("PRAGMA application_id="+str(APPLICATION_ID))
    db.execute("INSERT INTO store_meta VALUES(1,1,?,'READY')", (EPOCH,))
    db.execute("INSERT INTO operation_heads VALUES(?,?,2,1,1)", (EPOCH,OP))
    db.execute("""INSERT INTO binding_references(epoch,operation_id,generation,binding_sha256,
        envelope_schema,envelope,commit_result,state) VALUES(?,?,1,?,1,?,'committed','Active')""",
        (EPOCH,OP,BINDING,json.dumps(ENVELOPE,separators=(",", ":")).encode()))
    if not missing_create:
        db.execute("INSERT INTO lifecycle_events VALUES(?,?,1,1,?,'Create',NULL)", (EPOCH,OP,BINDING))
    db.execute("INSERT INTO lifecycle_events VALUES(?,?,2,1,?,'Activate',NULL)", (EPOCH,OP,BINDING))
    db.execute("COMMIT")

def semantic_check(db):
    """Narrow fixture replay; Rust harness additionally compares the pure model.

    Neither SQLite checks nor this test function authenticate an envelope or
    principal. This is deliberately NOT a production recovery implementation.
    """
    if db.execute("SELECT count(*) FROM operation_heads").fetchone() != (1,) or db.execute("SELECT count(*) FROM binding_references").fetchone() != (1,):
        raise ValueError("semantic_cardinality")
    head, active = db.execute("SELECT head,active_generation FROM operation_heads").fetchone()
    if not 1 <= head <= 1024:
        raise ValueError("semantic_head_bound")
    state, consumed, encoded, binding, commit = db.execute("SELECT state,consumed_claim_id,envelope,binding_sha256,commit_result FROM binding_references").fetchone()
    env = json.loads(encoded)
    if env != ENVELOPE or binding != BINDING or commit != env["payload"]["commit"]["result"]:
        raise ValueError("semantic_envelope")
    events = db.execute("SELECT sequence,action,claim_id FROM lifecycle_events ORDER BY sequence").fetchall()
    if [row[0] for row in events] != list(range(1,head+1)):
        raise ValueError("semantic_history")
    actions = [row[1] for row in events]
    claims = db.execute("SELECT claim_id,expected_head,committed_head FROM claims").fetchall()
    for claim_id, expected, committed in claims:
        row = db.execute("SELECT epoch,operation_id,generation,binding_sha256,producer_id,purpose,request_sha256 FROM claims WHERE claim_id=?",(claim_id,)).fetchone()
        req = request(claim=claim_id, expected_head=expected, next_head=committed)
        if row != (EPOCH,OP,1,BINDING,PRODUCER,"associate_boot_evidence",req["request_hash"]):
            raise ValueError("semantic_claim")
    if actions == ["Create","Activate"] and state == "Active" and active == 1 and consumed is None and not claims:
        return
    if (actions == ["Create","Activate","Consume"] and state == "Consumed" and active is None
        and claims == [(consumed,2,3)] and events[-1][2] == consumed):
        return
    raise ValueError("semantic_state")

class Rejected(Exception):
    pass

def schema_layout(db):
    objects = db.execute("SELECT type,name,tbl_name,sql FROM sqlite_schema ORDER BY type,name").fetchall()
    tables = [row[1] for row in objects if row[0] == "table"]
    details = []
    # Names are quoted and originate from sqlite_schema, never interpolated raw.
    for name in tables:
        quoted = '"'+name.replace('"','""')+'"'
        details.append((name, db.execute("PRAGMA table_xinfo("+quoted+")").fetchall(),
            db.execute("PRAGMA foreign_key_list("+quoted+")").fetchall(),
            db.execute("PRAGMA index_list("+quoted+")").fetchall()))
    indexes = [row[1] for row in objects if row[0] == "index"]
    for name in indexes:
        quoted = '"'+name.replace('"','""')+'"'
        details.append((name,db.execute("PRAGMA index_xinfo("+quoted+")").fetchall()))
    return objects, details

def expected_layout(legacy=False):
    # Trust anchor is the checked-in SQL fixture, NOT the candidate database's
    # own stored SQL/hash/version. This in-memory template contains no user data.
    if legacy not in _EXPECTED_LAYOUTS:
        expected = sqlite3.connect(":memory:")
        try:
            expected.executescript(SCHEMA + (LEGACY_MARKER if legacy else ""))
            _EXPECTED_LAYOUTS[legacy] = schema_layout(expected)
        finally:
            expected.close()
    return _EXPECTED_LAYOUTS[legacy]

def schema_check(db, version=1, legacy=False):
    for setting, expected in [("foreign_keys",1),("trusted_schema",0),("synchronous",3),
        ("journal_mode","delete"),("ignore_check_constraints",0)]:
        if db.execute("PRAGMA "+setting).fetchone() != (expected,):
            raise Rejected("configuration_mismatch")
    if not 0 < db.execute("PRAGMA busy_timeout").fetchone()[0] <= 2000:
        raise Rejected("configuration_mismatch")
    if not db.getconfig(sqlite3.SQLITE_DBCONFIG_ENABLE_TRIGGER):
        raise Rejected("schema_trigger_disabled")
    if db.execute("PRAGMA user_version").fetchone() != (version,) or db.execute("PRAGMA application_id").fetchone() != (APPLICATION_ID,):
        raise Rejected("schema_version_mismatch")
    if schema_layout(db) != expected_layout(legacy):
        raise Rejected("schema_identity_mismatch")
    if db.execute("SELECT singleton,schema_version,epoch,admission_state FROM store_meta").fetchall() != [(1,1,EPOCH,"READY")]:
        raise Rejected("schema_metadata_mismatch")
    if legacy and db.execute("SELECT version FROM fixture_legacy_marker").fetchall() != [(0,)]:
        raise Rejected("migration_unsupported")

def migrate_fixture(db, fault=None):
    """TEST-ONLY stamp migration: known v0 has v1 data layout plus a marker.

    Not a production legacy format or authority enrollment path. No constraints
    are disabled, no existing claims/references are rewritten or backfilled.
    """
    try:
        db.execute("BEGIN IMMEDIATE")
        current = db.execute("PRAGMA user_version").fetchone()[0]
        if current == 1:
            schema_check(db)
            db.execute("ROLLBACK")
            return "migration_already_current"
        if current != 0:
            raise Rejected("migration_unsupported")
        schema_check(db,version=0,legacy=True)
        semantic_check(db)
        # execute(), not executescript(): executescript can commit implicitly.
        db.execute("DROP TABLE fixture_legacy_marker")
        db.execute("PRAGMA user_version=1")
        if fault == "error":
            raise Rejected("migration_interrupted")
        if fault == "close":
            db.close()
            return "migration_connection_closed"
        schema_check(db)
        if db.execute("PRAGMA foreign_key_check").fetchall():
            raise Rejected("migration_integrity")
        db.execute("COMMIT")
        return "migration_committed"
    except sqlite3.DatabaseError:
        if db.in_transaction:
            db.execute("ROLLBACK")
        raise Rejected("migration_store_unavailable") from None
    except Exception:
        if db.in_transaction:
            db.execute("ROLLBACK")
        raise

class Adapter:
    def __init__(self, db):
        self.db = db
        self.closed_gate = True

    def claim(self, req, fault=None, authorized=True):
        if not authorized:
            raise Rejected("claim_unauthorized")
        if self.closed_gate:
            raise Rejected("claim_indeterminate")
        db = self.db
        try:
            db.execute("BEGIN IMMEDIATE")
            try:
                schema_check(db)
            except Rejected:
                self.closed_gate = True
                raise
            semantic_check(db)
            old = db.execute("""SELECT epoch,operation_id,generation,binding_sha256,expected_head,
                committed_head,producer_id,purpose,request_sha256 FROM claims WHERE claim_id=?""",(req["claim"],)).fetchone()
            expected = tuple(req[k] for k in ("epoch","op","gen","binding","expected_head","next_head","producer")) + ("associate_boot_evidence",req["request_hash"])
            if old is not None:
                if old != expected:
                    raise Rejected("claim_conflict")
                db.execute("ROLLBACK")
                return old[5]
            for index, sql in enumerate(STEPS):
                cursor = db.execute(sql, req)
                if cursor.rowcount != 1:
                    raise Rejected("claim_stale_or_ineligible")
                if fault == "rollback_"+str(index):
                    raise Rejected("synthetic_precommit_crash")
            semantic_check(db)
            if fault == "io_before":
                raise OSError("synthetic_io")
            if fault == "exit_before":
                os._exit(17)
            db.execute("COMMIT")
            if fault == "exit_after":
                os._exit(17)
            if fault == "io_after":
                raise OSError("synthetic_io")
            if fault == "lost_response":
                raise Rejected("claim_response_lost")
            return req["next_head"]
        except (sqlite3.DatabaseError, OSError, ValueError):
            self.closed_gate = True
            if db.in_transaction:
                db.execute("ROLLBACK")
            raise
        except Exception:
            if db.in_transaction:
                db.execute("ROLLBACK")
            raise

    def query(self, key):
        if self.closed_gate:
            raise Rejected("claim_indeterminate")
        try:
            schema_check(self.db)
            semantic_check(self.db)
            return self.db.execute("SELECT committed_head FROM claims WHERE claim_id=? AND producer_id=?",(key,PRODUCER)).fetchone()
        except (Rejected, ValueError):
            self.closed_gate = True
            raise
        except sqlite3.DatabaseError:
            self.closed_gate = True
            raise Rejected("recovery_store_unavailable") from None

    def recover(self, assumed_current=False):
        self.closed_gate = True
        if not assumed_current:
            raise Rejected("freshness_not_established")
        try:
            self.db.execute("BEGIN")
            schema_check(self.db)
            if self.db.execute("PRAGMA integrity_check").fetchall() != [("ok",)]:
                raise Rejected("integrity_failure")
            if self.db.execute("PRAGMA foreign_key_check").fetchall():
                raise Rejected("foreign_key_failure")
            semantic_check(self.db)
            self.db.execute("COMMIT")
        except sqlite3.DatabaseError:
            if self.db.in_transaction:
                self.db.execute("ROLLBACK")
            raise Rejected("recovery_store_unavailable") from None
        except Exception:
            if self.db.in_transaction:
                self.db.execute("ROLLBACK")
            raise
        self.closed_gate = False

def admitted(db):
    adapter = Adapter(db)
    adapter.recover(assumed_current=True)  # Explicit fixture assumption only.
    return adapter

class SqliteClaimTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zethropol-claim-sqlite-")
        self.path = Path(self.temp.name)/"claim.db"
        fd = os.open(self.path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        os.close(fd)
        self.connections = []
        self.db = self.open()
        self.db.executescript(SCHEMA)
        seed(self.db)
        self.adapter = admitted(self.db)

    def open(self, timeout=2000):
        db = connect(self.path,timeout)
        self.connections.append(db)
        return db

    def tearDown(self):
        for db in self.connections:
            db.close()
        self.temp.cleanup()
        self.assertFalse(self.path.parent.exists())

    def healthy(self):
        self.assertEqual(self.db.execute("PRAGMA integrity_check").fetchall(),[("ok",)])
        self.assertEqual(self.db.execute("PRAGMA foreign_key_check").fetchall(),[])
        semantic_check(self.db)

    def active(self):
        self.assertEqual(self.db.execute("SELECT head,active_generation FROM operation_heads").fetchone(),(2,1))
        self.assertEqual(self.db.execute("SELECT state FROM binding_references").fetchone(),("Active",))
        self.assertEqual(self.db.execute("SELECT count(*) FROM claims").fetchone(),(0,))
        self.healthy()

    def test_configuration(self):
        for key,value in [("foreign_keys",1),("journal_mode","delete"),("synchronous",3),
                          ("trusted_schema",0),("busy_timeout",2000)]:
            self.assertEqual(self.db.execute("PRAGMA "+key).fetchone(),(value,))
        self.healthy()

    def test_schema_exactly_matches_design(self):
        document = HERE.parents[1]/"ATOMIC_CLAIM_SQLITE_DESIGN.md"
        blocks = document.read_text().split("```sql\n")
        self.assertEqual(SCHEMA,blocks[1].split("```")[0])
        self.assertEqual((HERE/"atomic_claim_transaction.sql").read_text(),blocks[3].split("```")[0])

    def test_primary_check_strict_and_immediate_foreign_constraints(self):
        for sql,args in [
            ("INSERT INTO store_meta VALUES(1,1,?,'READY')",(OTHER,)),
            ("INSERT INTO store_meta VALUES(2,1,?,'READY')",(OTHER,)),
            ("INSERT INTO store_meta VALUES(1,2,?,'READY')",(OTHER,)),
            ("INSERT INTO store_meta VALUES(1,1,?,'READY')",(b"short",)),
            ("UPDATE operation_heads SET head='invalid'",()),
            ("INSERT INTO lifecycle_events VALUES(?,?,3,999,?,'Create',NULL)",(EPOCH,OP,BINDING)),
        ]:
            with self.subTest(sql=sql):
                with self.assertRaises(sqlite3.IntegrityError): self.db.execute(sql,args)
        self.active()

    def test_second_active_unique_index(self):
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("""INSERT INTO binding_references(epoch,operation_id,generation,binding_sha256,
                envelope_schema,envelope,commit_result,state) VALUES(?,?,2,?,1,?,'committed','Active')""",
                (EPOCH,OP,BINDING,b"{}"))
        self.active()

    def test_deferred_missing_claim_event_or_consumed_reference_fails_commit(self):
        for omitted in [0,2,3]:
            self.db.execute("BEGIN IMMEDIATE")
            for index,sql in enumerate(STEPS):
                if index != omitted: self.db.execute(sql,request())
            with self.assertRaises(sqlite3.IntegrityError): self.db.execute("COMMIT")
            self.assertTrue(self.db.in_transaction)
            self.db.execute("ROLLBACK")
            self.active()

    def test_missing_head_update_rejected_by_database(self):
        self.db.execute("BEGIN IMMEDIATE")
        self.db.execute(STEPS[0],request())
        self.db.execute(STEPS[2],request())
        with self.assertRaisesRegex(sqlite3.IntegrityError,"consume_head_mismatch"):
            self.db.execute(STEPS[3],request())
        self.db.execute("ROLLBACK")
        self.active()

    def test_fixture_directory_and_file_are_private(self):
        import stat
        self.assertEqual(stat.S_IMODE(self.path.parent.stat().st_mode),0o700)
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode),0o600)
        self.assertEqual(self.path.stat().st_nlink,1)

    def test_deferred_relationship_completed_before_commit(self):
        self.db.execute("BEGIN IMMEDIATE")
        self.db.execute(STEPS[0],request())
        self.assertTrue(self.db.execute("PRAGMA foreign_key_check").fetchall())
        for sql in STEPS[1:]: self.assertEqual(self.db.execute(sql,request()).rowcount,1)
        self.db.execute("COMMIT")
        self.assertEqual(self.adapter.query(CLAIM),(3,))
        self.healthy()

    def test_atomic_claim_and_idempotent_reopen_query(self):
        self.assertEqual(self.adapter.claim(request()),3)
        self.assertEqual(self.adapter.claim(request()),3)
        self.assertEqual(self.db.execute("SELECT count(*) FROM claims").fetchone(),(1,))
        self.assertEqual(self.db.execute("SELECT head FROM operation_heads").fetchone(),(3,))
        self.assertEqual(admitted(self.open()).query(CLAIM),(3,))
        self.healthy()

    def test_duplicate_claim_id_changed_input_rejected(self):
        self.adapter.claim(request())
        for changes in [dict(expected_head=3,next_head=4),dict(gen=2),dict(binding=b"x"*32),dict(producer=OTHER)]:
            with self.assertRaisesRegex(Rejected,"claim_conflict"):
                self.adapter.claim(request(**changes))
        # Direct INSERT also fails, even if adapter is bypassed.
        with self.assertRaises(sqlite3.IntegrityError): self.db.execute(STEPS[2],request())
        self.healthy()

    def test_second_consumption_rejected_by_database(self):
        self.adapter.claim(request())
        with self.assertRaises(sqlite3.IntegrityError): self.db.execute(STEPS[2],request(claim=OTHER))
        self.db.execute("BEGIN IMMEDIATE")
        self.db.execute("UPDATE operation_heads SET head=4")
        with self.assertRaises(sqlite3.IntegrityError) as caught:
            self.db.execute(STEPS[3],request(claim=OTHER,next_head=4))
        self.assertEqual(caught.exception.sqlite_errorname,"SQLITE_CONSTRAINT_UNIQUE")
        self.db.execute("ROLLBACK")
        self.healthy()

    def test_stale_head_generation_binding_and_operation_roll_back(self):
        for changes in [dict(expected_head=1,next_head=2),dict(gen=2),dict(binding=b"x"*32),dict(op=OTHER)]:
            with self.assertRaisesRegex(Rejected,"stale_or_ineligible"): self.adapter.claim(request(**changes))
            self.active()

    def test_every_precommit_step_rolls_back(self):
        for index in range(4):
            with self.assertRaisesRegex(Rejected,"precommit_crash"):
                self.adapter.claim(request(),fault="rollback_"+str(index))
            self.active()

    def test_other_connection_sees_no_intermediate_writes(self):
        other = self.open()
        self.db.execute("BEGIN IMMEDIATE")
        for sql in STEPS: self.db.execute(sql,request())
        self.assertEqual(other.execute("SELECT count(*) FROM claims").fetchone(),(0,))
        self.assertEqual(other.execute("SELECT state FROM binding_references").fetchone(),("Active",))
        self.db.execute("ROLLBACK")
        self.active()

    def test_two_connections_race_exactly_one_winner(self):
        barrier = threading.Barrier(2)
        outcomes = []
        def contender(key):
            db = connect(self.path)
            try:
                barrier.wait(timeout=5)
                try: outcomes.append(admitted(db).claim(request(claim=key)))
                except (Rejected,sqlite3.DatabaseError) as error: outcomes.append(type(error).__name__)
            finally: db.close()
        threads = [threading.Thread(target=contender,args=(key,)) for key in [CLAIM,OTHER]]
        for thread in threads: thread.start()
        for thread in threads:
            thread.join(timeout=10)
            self.assertFalse(thread.is_alive())
        self.assertEqual(outcomes.count(3),1,outcomes)
        self.assertEqual(len(outcomes),2)
        self.assertEqual(self.db.execute("SELECT count(*) FROM claims").fetchone(),(1,))
        self.healthy()

    def test_busy_writer_never_reports_success(self):
        other = self.open(timeout=25)
        contender = admitted(other)
        self.db.execute("BEGIN IMMEDIATE")
        with self.assertRaises(sqlite3.OperationalError) as caught:
            contender.claim(request())
        self.assertEqual(caught.exception.sqlite_errorcode,sqlite3.SQLITE_BUSY)
        self.db.execute("ROLLBACK")
        self.active()

    def test_active_predicate_rejects_revoked_and_candidate_without_consuming(self):
        self.db.execute("BEGIN IMMEDIATE")
        self.db.execute("UPDATE binding_references SET state='Revoked'")
        self.assertEqual(self.db.execute(STEPS[0],request()).rowcount,0)
        self.db.execute("ROLLBACK")
        self.db.execute("BEGIN IMMEDIATE")
        self.db.execute("""INSERT INTO binding_references(epoch,operation_id,generation,binding_sha256,
            envelope_schema,envelope,commit_result,state) VALUES(?,?,2,?,1,?,'committed','Candidate')""",
            (EPOCH,OP,BINDING,b"{}"))
        self.assertEqual(self.db.execute(STEPS[0],request(gen=2)).rowcount,0)
        self.db.execute("ROLLBACK")
        self.active()

    def test_busy_at_commit_rolls_back_and_closes_admission(self):
        reader = self.open()
        reader.execute("BEGIN")
        reader.execute("SELECT head FROM operation_heads").fetchall()
        self.db.execute("PRAGMA busy_timeout=25")
        with self.assertRaises(sqlite3.OperationalError) as caught:
            self.adapter.claim(request())
        self.assertEqual(caught.exception.sqlite_errorcode,sqlite3.SQLITE_BUSY)
        self.assertTrue(self.adapter.closed_gate)
        reader.execute("ROLLBACK")
        self.active()
        with self.assertRaisesRegex(Rejected,"indeterminate"):
            self.adapter.claim(request(claim=OTHER))
        self.adapter.recover(True)
        self.assertEqual(self.adapter.query(CLAIM),None)

    def test_commit_success_response_lost(self):
        with self.assertRaisesRegex(Rejected,"response_lost"):
            self.adapter.claim(request(),fault="lost_response")
        self.assertEqual(admitted(self.open()).query(CLAIM),(3,))
        self.healthy()

    def test_process_exit_before_and_after_commit(self):
        for fault in ["exit_before","exit_after"]:
            result = subprocess.run([sys.executable,"-I","-B",str(Path(__file__).resolve()),
                "--child",str(self.path),fault],capture_output=True,timeout=10)
            self.assertEqual(result.returncode,17,result.stderr.decode())
            if fault == "exit_before": self.active()
            else: self.assertEqual(admitted(self.open()).query(CLAIM),(3,))
        self.healthy()

    def test_synthetic_io_uncertainty_blocks_admission_until_explicit_recovery(self):
        for fault in ["io_before","io_after"]:
            with self.assertRaises(OSError): self.adapter.claim(request(),fault=fault)
            with self.assertRaisesRegex(Rejected,"indeterminate"): self.adapter.claim(request(claim=OTHER))
            with self.assertRaisesRegex(Rejected,"indeterminate"): self.adapter.query(CLAIM)
            with self.assertRaisesRegex(Rejected,"freshness_not_established"): self.adapter.recover()
            self.adapter.recover(assumed_current=True)  # Synthetic assumption only.
            self.assertEqual(self.adapter.query(CLAIM),None if fault=="io_before" else (3,))
        self.healthy()

    def test_real_sqlite_full_from_fixture_page_limit(self):
        self.db.execute("CREATE TABLE fixture_fill(value BLOB) STRICT")
        pages = self.db.execute("PRAGMA page_count").fetchone()[0]
        self.db.execute("PRAGMA max_page_count="+str(pages))
        with self.assertRaises(sqlite3.OperationalError) as caught:
            self.db.execute("INSERT INTO fixture_fill VALUES(zeroblob(1048576))")
        self.assertEqual(caught.exception.sqlite_errorcode,sqlite3.SQLITE_FULL)
        self.db.execute("DROP TABLE fixture_fill")
        self.active()

    def test_sqlite_integrity_is_not_semantic_completeness(self):
        # All FKs and B-trees are intact, but event says Revoke while projection
        # remains Active. Generic SQLite checks cannot validate this domain rule.
        self.db.execute("BEGIN IMMEDIATE")
        self.db.execute("INSERT INTO lifecycle_events VALUES(?,?,3,1,?,'Revoke',NULL)",(EPOCH,OP,BINDING))
        self.db.execute("UPDATE operation_heads SET head=3")
        self.db.execute("COMMIT")
        self.assertEqual(self.db.execute("PRAGMA integrity_check").fetchall(),[("ok",)])
        self.assertEqual(self.db.execute("PRAGMA foreign_key_check").fetchall(),[])
        with self.assertRaisesRegex(ValueError,"semantic_state"): self.adapter.recover(True)
        self.assertTrue(self.adapter.closed_gate)

    def test_missing_history_in_independent_store(self):
        # Independent DB for this test; rebuild empty schema within its own temp
        # namespace, never repair a production or corrupted store.
        for db in self.connections: db.close()
        self.connections=[]
        self.path.unlink()
        fd=os.open(self.path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600);os.close(fd)
        self.db=self.open();self.db.executescript(SCHEMA);seed(self.db,missing_create=True)
        self.assertEqual(self.db.execute("PRAGMA foreign_key_check").fetchall(),[])
        with self.assertRaisesRegex(ValueError,"semantic_history"): Adapter(self.db).recover(True)

    def test_corrupt_and_missing_database_are_not_recreated(self):
        for db in self.connections: db.close()
        self.connections=[]
        self.path.write_bytes(b"not a sqlite database")
        with self.assertRaises(sqlite3.DatabaseError): connect(self.path)
        self.path.unlink()
        with self.assertRaises(sqlite3.OperationalError): connect(self.path)
        self.assertFalse(self.path.exists())

    def test_unauthorized_fixture_principal_and_immutable_rows(self):
        with self.assertRaisesRegex(Rejected,"unauthorized"):
            self.adapter.claim(request(),authorized=False)
        self.active()
        self.adapter.claim(request())
        for sql in ["UPDATE claims SET purpose='associate_boot_evidence'", "DELETE FROM claims",
                    "UPDATE lifecycle_events SET action='Create'", "DELETE FROM lifecycle_events",
                    "UPDATE binding_references SET envelope=x'00'", "UPDATE binding_references SET state='Active'"]:
            with self.assertRaises(sqlite3.IntegrityError): self.db.execute(sql)
        self.healthy()

    def denied_recovery(self, code):
        # Failed recovery must not mutate or repair the candidate schema/data.
        before = schema_layout(self.db)
        changes = self.db.total_changes
        adapter = Adapter(self.db)
        with self.assertRaises((Rejected,ValueError)) as caught:
            adapter.recover(assumed_current=True)
        self.assertEqual(str(caught.exception),code)
        self.assertEqual(self.db.total_changes,changes)
        self.assertEqual(schema_layout(self.db),before)
        self.assertTrue(adapter.closed_gate)
        with self.assertRaisesRegex(Rejected,"claim_indeterminate"):
            adapter.claim(request())
        with self.assertRaisesRegex(Rejected,"claim_indeterminate"):
            adapter.query(CLAIM)

    def rebuild_fixture(self, schema):
        # Negative-test provisioning only. Admission never repairs schemas.
        for db in self.connections: db.close()
        self.connections=[]
        self.path.unlink()
        fd=os.open(self.path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600);os.close(fd)
        self.db=self.open();self.db.executescript(schema);seed(self.db)
        self.adapter=Adapter(self.db)

    def legacy_fixture(self):
        self.adapter.closed_gate=True
        self.db.execute("BEGIN IMMEDIATE")
        self.db.execute(LEGACY_MARKER)
        self.db.execute("INSERT INTO fixture_legacy_marker VALUES(0)")
        self.db.execute("PRAGMA user_version=0")
        self.db.execute("COMMIT")

    def test_admission_starts_closed_and_needs_independent_freshness_assumption(self):
        adapter=Adapter(self.db)
        with self.assertRaisesRegex(Rejected,"claim_indeterminate"): adapter.claim(request())
        with self.assertRaisesRegex(Rejected,"freshness_not_established"): adapter.recover()
        self.assertTrue(adapter.closed_gate)
        schema_check(self.db)  # Matching schema is not a freshness authority.
        adapter.recover(True)
        self.assertFalse(adapter.closed_gate)

    def test_wrong_user_version_and_application_id(self):
        self.db.execute("PRAGMA user_version=99")
        self.denied_recovery("schema_version_mismatch")
        self.db.execute("PRAGMA user_version=1")
        self.db.execute("PRAGMA application_id=0")
        self.denied_recovery("schema_version_mismatch")

    def test_wrong_metadata_version_is_not_trusted(self):
        self.db.execute("PRAGMA ignore_check_constraints=ON")
        self.db.execute("UPDATE store_meta SET schema_version=99")
        self.db.execute("PRAGMA ignore_check_constraints=OFF")
        self.denied_recovery("schema_metadata_mismatch")

    def test_missing_table_and_column(self):
        self.db.execute("PRAGMA foreign_keys=OFF")
        self.db.execute("DROP TABLE claims")
        self.db.execute("PRAGMA foreign_keys=ON")
        self.denied_recovery("schema_identity_mismatch")
        self.rebuild_fixture(SCHEMA.replace("  request_sha256 BLOB NOT NULL CHECK(length(request_sha256) = 32),\n", ""))
        self.denied_recovery("schema_identity_mismatch")

    def test_missing_unique_check_and_foreign_key_constraints(self):
        for old,new in [
            ("  UNIQUE(epoch, operation_id, generation),\n", ""),
            ("schema_version = 1", "schema_version >= 1"),
            ("  FOREIGN KEY(epoch) REFERENCES store_meta(epoch),\n", ""),
        ]:
            self.rebuild_fixture(SCHEMA.replace(old,new))
            self.denied_recovery("schema_identity_mismatch")

    def test_missing_indexes_and_unexpected_schema_object(self):
        self.db.execute("DROP INDEX one_consumption")
        self.denied_recovery("schema_identity_mismatch")
        self.rebuild_fixture(SCHEMA)
        self.db.execute("CREATE TABLE unexpected_token_table(value TEXT)")
        self.denied_recovery("schema_identity_mismatch")

    def test_missing_or_noop_consume_head_trigger(self):
        self.db.execute("DROP TRIGGER consume_matches_head")
        self.denied_recovery("schema_identity_mismatch")
        self.db.execute("""CREATE TRIGGER consume_matches_head BEFORE INSERT ON lifecycle_events
            WHEN 0 BEGIN SELECT RAISE(ABORT,'unused'); END""")
        self.denied_recovery("schema_identity_mismatch")

    def test_runtime_trigger_disable_and_constraint_disable_are_rejected(self):
        self.db.setconfig(sqlite3.SQLITE_DBCONFIG_ENABLE_TRIGGER,False)
        self.denied_recovery("schema_trigger_disabled")
        self.db.setconfig(sqlite3.SQLITE_DBCONFIG_ENABLE_TRIGGER,True)
        self.db.execute("PRAGMA ignore_check_constraints=ON")
        self.denied_recovery("configuration_mismatch")
        self.db.execute("PRAGMA ignore_check_constraints=OFF")

    def test_schema_tamper_after_admission_closes_gate_before_claim(self):
        self.db.execute("DROP TRIGGER consume_matches_head")
        with self.assertRaisesRegex(Rejected,"schema_identity_mismatch"):
            self.adapter.claim(request())
        self.assertTrue(self.adapter.closed_gate)
        self.assertEqual(self.db.execute("SELECT count(*) FROM claims").fetchone(),(0,))

    def test_foreign_key_corruption_is_rejected_with_exact_schema(self):
        self.db.execute("PRAGMA foreign_keys=OFF")
        self.db.execute("INSERT INTO lifecycle_events VALUES(?,?,3,999,?,'Create',NULL)",(EPOCH,OP,BINDING))
        self.db.execute("PRAGMA foreign_keys=ON")
        self.denied_recovery("foreign_key_failure")

    def test_claim_without_consumption_is_rejected_on_recovery(self):
        self.db.execute("PRAGMA foreign_keys=OFF")
        self.db.execute(STEPS[2],request())
        self.db.execute("PRAGMA foreign_keys=ON")
        self.denied_recovery("foreign_key_failure")

    def test_consumption_without_claim_is_rejected_on_recovery(self):
        self.db.execute("PRAGMA foreign_keys=OFF")
        self.db.execute("BEGIN IMMEDIATE")
        self.db.execute("UPDATE operation_heads SET head=3,active_generation=NULL")
        self.db.execute(STEPS[3],request())
        self.db.execute("COMMIT")
        self.db.execute("PRAGMA foreign_keys=ON")
        self.denied_recovery("foreign_key_failure")

    def test_conflicting_active_references_cannot_open_admission(self):
        self.db.execute("DROP INDEX one_active_reference")
        self.db.execute("""INSERT INTO binding_references(epoch,operation_id,generation,binding_sha256,
            envelope_schema,envelope,commit_result,state) VALUES(?,?,2,?,1,?,'committed','Active')""",
            (EPOCH,OP,BINDING,b"{}"))
        self.assertEqual(self.db.execute("PRAGMA integrity_check").fetchall(),[("ok",)])
        self.denied_recovery("schema_identity_mismatch")
        with self.assertRaisesRegex(ValueError,"semantic_cardinality"): semantic_check(self.db)

    def test_head_overflow_is_bounded_before_history_allocation(self):
        self.db.setconfig(sqlite3.SQLITE_DBCONFIG_ENABLE_TRIGGER,False)
        self.db.execute("PRAGMA foreign_keys=OFF")
        self.db.execute("INSERT INTO lifecycle_events VALUES(?,?,9223372036854775807,1,?,'Revoke',NULL)",(EPOCH,OP,BINDING))
        self.db.execute("UPDATE operation_heads SET head=9223372036854775807")
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.setconfig(sqlite3.SQLITE_DBCONFIG_ENABLE_TRIGGER,True)
        self.assertEqual(self.db.execute("PRAGMA foreign_key_check").fetchall(),[])
        self.denied_recovery("semantic_head_bound")

    def test_inconsistent_active_pointer_is_not_an_integrity_pragma_check(self):
        self.db.execute("BEGIN IMMEDIATE")
        self.db.execute("INSERT INTO lifecycle_events VALUES(?,?,3,1,?,'Revoke',NULL)",(EPOCH,OP,BINDING))
        self.db.execute("UPDATE operation_heads SET head=3,active_generation=NULL")
        self.db.execute("COMMIT")
        self.assertEqual(self.db.execute("PRAGMA integrity_check").fetchall(),[("ok",)])
        self.denied_recovery("semantic_state")

    def test_integrity_check_failure_is_sanitized_and_closes_gate(self):
        self.db.setconfig(sqlite3.SQLITE_DBCONFIG_ENABLE_TRIGGER,False)
        self.db.execute("PRAGMA ignore_check_constraints=ON")
        self.db.execute("UPDATE binding_references SET commit_result='invalid'")
        self.db.execute("PRAGMA ignore_check_constraints=OFF")
        self.db.setconfig(sqlite3.SQLITE_DBCONFIG_ENABLE_TRIGGER,True)
        self.assertNotEqual(self.db.execute("PRAGMA integrity_check").fetchall(),[("ok",)])
        self.denied_recovery("integrity_failure")

    def test_semantic_claim_hash_mismatch_is_not_sqlite_integrity(self):
        self.adapter.claim(request())
        self.db.setconfig(sqlite3.SQLITE_DBCONFIG_ENABLE_TRIGGER,False)
        self.db.execute("UPDATE claims SET request_sha256=zeroblob(32)")
        self.db.setconfig(sqlite3.SQLITE_DBCONFIG_ENABLE_TRIGGER,True)
        self.assertEqual(self.db.execute("PRAGMA integrity_check").fetchall(),[("ok",)])
        self.assertEqual(self.db.execute("PRAGMA foreign_key_check").fetchall(),[])
        self.denied_recovery("semantic_claim")

    def test_recovery_required_metadata_blocks_admission(self):
        self.db.execute("UPDATE store_meta SET admission_state='RECOVERY_REQUIRED'")
        self.denied_recovery("schema_metadata_mismatch")

    def test_supported_fixture_stamp_migration_is_atomic_and_idempotent(self):
        self.adapter.claim(request())
        self.legacy_fixture()
        self.denied_recovery("schema_version_mismatch")
        self.assertEqual(migrate_fixture(self.db),"migration_committed")
        self.assertEqual(migrate_fixture(self.db),"migration_already_current")
        self.adapter.recover(True)
        self.assertEqual(self.adapter.query(CLAIM),(3,))
        self.assertEqual(self.db.execute("SELECT count(*) FROM claims").fetchone(),(1,))
        self.healthy()

    def test_unknown_newer_and_unmarked_legacy_migrations_fail_closed(self):
        for version in [2,99,0]:
            self.db.execute("PRAGMA user_version="+str(version))
            before=schema_layout(self.db)
            with self.assertRaises(Rejected) as caught: migrate_fixture(self.db)
            self.assertEqual(str(caught.exception),"schema_identity_mismatch" if version==0 else "migration_unsupported")
            self.assertEqual(schema_layout(self.db),before)
            self.assertEqual(self.db.execute("PRAGMA user_version").fetchone(),(version,))
            self.denied_recovery("schema_version_mismatch")

    def test_migration_error_rolls_back_all_ddl_and_version_changes(self):
        self.legacy_fixture()
        before=schema_layout(self.db)
        with self.assertRaisesRegex(Rejected,"migration_interrupted"): migrate_fixture(self.db,"error")
        self.assertEqual(schema_layout(self.db),before)
        self.assertEqual(self.db.execute("PRAGMA user_version").fetchone(),(0,))
        self.denied_recovery("schema_version_mismatch")
        self.assertEqual(migrate_fixture(self.db),"migration_committed")
        self.adapter.recover(True)
        self.active()

    def test_connection_close_during_migration_leaves_legacy_schema(self):
        self.legacy_fixture()
        before=schema_layout(self.db)
        self.assertEqual(migrate_fixture(self.db,"close"),"migration_connection_closed")
        self.db=self.open()
        self.assertEqual(schema_layout(self.db),before)
        self.assertEqual(self.db.execute("PRAGMA user_version").fetchone(),(0,))
        self.denied_recovery("schema_version_mismatch")
        self.assertEqual(migrate_fixture(self.db),"migration_committed")
        admitted(self.db).query(CLAIM)

    def test_partial_migration_is_neither_current_nor_known_legacy(self):
        self.legacy_fixture()
        self.db.execute("PRAGMA user_version=1")  # Simulated external partial migration.
        self.denied_recovery("schema_identity_mismatch")
        with self.assertRaisesRegex(Rejected,"schema_identity_mismatch"): migrate_fixture(self.db)
        self.assertEqual(self.db.execute("SELECT version FROM fixture_legacy_marker").fetchall(),[(0,)])

    def test_rollback_and_old_head_do_not_become_fresh_from_schema_match(self):
        # Backup is fixture data only. Both old head and schema can be restored.
        old=sqlite3.connect(":memory:")
        try:
            self.db.backup(old)
            self.adapter.claim(request())
            self.adapter.closed_gate=True
            old.backup(self.db)
        finally: old.close()
        schema_check(self.db)
        with self.assertRaisesRegex(Rejected,"freshness_not_established"): self.adapter.recover()
        self.assertTrue(self.adapter.closed_gate)
        # Counterexample: falsely assuming freshness still accepts a restored DB.
        # This is why no production authority is enabled by these tests.
        self.adapter.recover(True)
        self.assertEqual(self.adapter.query(CLAIM),None)

if __name__ == "__main__":
    if len(sys.argv)>1 and sys.argv[1]=="--child":
        db=connect(Path(sys.argv[2]));admitted(db).claim(request(),fault=sys.argv[3]);raise SystemExit(99)
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(SqliteClaimTests)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print(json.dumps(dict(envelope=ENVELOPE,committed_head=3,claim_count=1,
            sqlite_version=sqlite3.sqlite_version,tests=result.testsRun)))
    raise SystemExit(0 if result.wasSuccessful() else 1)

"""Pure read-only acceptance contracts. Persist stat uint64s as decimal strings.

Never round-trip nanoseconds through JavaScript Number; never replace the
original baseline with current stat observations to make a failed guard pass.
"""
import re

FIELDS=('device','inode','mode','uid','gid','size','mtime_ns','ctime_ns')


def encode_snapshot(snapshot):
    rows={}
    for path,values in snapshot.items():
        if not isinstance(path,str) or not path.startswith('/') or len(values)!=len(FIELDS):
            raise ValueError('Invalid snapshot row')
        if any(type(v) is not int or not 0<=v<=2**64-1 for v in values):
            raise ValueError('Exact unsigned stat integers required')
        rows[path]=[str(v) for v in values]
    return {'schema':1,'encoding':'uint64-decimal-string','fields':list(FIELDS),'rows':rows}


def decode_snapshot(value,expected_paths):
    if (not isinstance(value,dict) or set(value)!={'schema','encoding','fields','rows'} or
            type(value['schema']) is not int or value['schema']!=1 or
            value['encoding']!='uint64-decimal-string' or value['fields']!=list(FIELDS) or
            not isinstance(value['rows'],dict) or set(value['rows'])!=set(expected_paths)):
        raise ValueError('Exact snapshot contract required')
    result={}
    for path,row in value['rows'].items():
        if (not isinstance(row,list) or len(row)!=len(FIELDS) or
                any(type(v) is not str or not re.fullmatch(r'0|[1-9][0-9]{0,19}',v) or int(v)>2**64-1 for v in row)):
            raise ValueError('Lossless decimal stat fields required')
        result[path]=tuple(int(v) for v in row)
    return result


def audit_contract(records,release,identity,count=16):
    if not isinstance(records,list) or any(not isinstance(row,dict) for row in records):
        raise ValueError('Invalid audit records')
    if [r.get('event') for r in records]!=['preflight-pass','staging-complete','published','success']:
        raise ValueError('Incomplete or ambiguous audit phases')
    if any(r.get('release')!=release for r in records):raise ValueError('Audit release mismatch')
    for row in records[1:3]:
        if type(row.get('dev')) is not int or type(row.get('ino')) is not int or (row['dev'],row['ino'])!=identity:
            raise ValueError('Audit inode mismatch')
    if type(records[-1].get('files_verified')) is not int or records[-1]['files_verified']!=count or records[-1].get('signing') is not False:
        raise ValueError('Exact non-signing audit success required')
    return True

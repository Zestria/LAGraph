#!/usr/bin/env python3
"""Read-only archive and payload verification; does not run benchmark code."""
from pathlib import Path
import hashlib,json,tarfile
root=Path(__file__).resolve().parent
for line in (root/'SHA256SUMS').read_text().splitlines():
    expected,name=line.split('  ',1)
    p=root/name
    assert p.is_file() and hashlib.sha256(p.read_bytes()).hexdigest()==expected,name
records={r['member']:r for r in map(json.loads,(root/'evidence_manifest.jsonl').read_text().splitlines())}
seen=set()
with tarfile.open(root/'evidence.tar.gz','r:gz') as tar:
    for member in tar:
        assert member.isfile() and member.name in records,member.name
        assert member.name not in seen,member.name
        record=records[member.name]
        data=tar.extractfile(member).read()
        assert len(data)==record['bytes'] and hashlib.sha256(data).hexdigest()==record['sha256'],member.name
        seen.add(member.name)
assert seen==set(records),'Missing members'
print(f'PASS: snapshot file checksums and {len(seen)} evidence members verified; no benchmark executed.')

#!/usr/bin/env python3
"""Restore the pinned NanoTrack v2 ONNX files and verify their SHA-256 hashes."""
import hashlib
import json
from pathlib import Path
import urllib.request

root = Path(__file__).resolve().parents[1] / 'models' / 'nanotrack'
manifest = json.loads((root / 'provenance.json').read_text())
for item in manifest['files']:
    target = root / item['name']
    if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == item['sha256']:
        print(f"Verified {item['name']}")
        continue
    data = urllib.request.urlopen(item['url'], timeout=60).read()
    if hashlib.sha256(data).hexdigest() != item['sha256']:
        raise SystemExit(f"Checksum mismatch: {item['name']}")
    target.write_bytes(data)
    print(f"Downloaded and verified {item['name']}")

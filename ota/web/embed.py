#!/usr/bin/env python3
"""Embed the offline management UI in the host server executable."""

import json
from pathlib import Path
import sys

web = Path(__file__).resolve().parent
sys.path.insert(0, str(web.parents[1] / 'identity'))
from registry import load_registry

registry = load_registry(web.parents[1] / 'config/hwid-registry.json')
page = (web / "index.html").read_text()
for marker, filename in (
    ("/* INLINE_STYLES */", "styles.css"),
    ("/* INLINE_SOL */", "sol-console.js"),
    ("/* INLINE_SOL_NAMES */", "sol-names.js"),
    ("/* INLINE_HWID */", "hwid.js"),
    ("/* INLINE_APP */", "app.js"),
    ("/* INLINE_FLEET */", "fleet.js"),
    ("/* INLINE_CLOCK */", "clock.js"),
    ("/* INLINE_SOL_RECORDS */", "sol-records.js"),
    ("/* INLINE_INCIDENT_LOGS */", "incident-logs.js"),
):
    assert page.count(marker) == 1, marker
    page = page.replace(marker, (web / filename).read_text())

lines = []
for symbol, text in (
    ('admin_page', page),
    ('hwid_registry_json', json.dumps(registry, ensure_ascii=False, separators=(',', ':')) + '\n'),
):
    data = text.encode('utf-8')
    lines.append(f'static const unsigned char {symbol}[] = {{')
    for pos in range(0, len(data), 32):
        lines.append("    " + ", ".join(str(byte) for byte in data[pos:pos + 32]) + ",")
    lines.append("    0\n};\n")
Path(sys.argv[1]).write_text("\n".join(lines))

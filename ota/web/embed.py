#!/usr/bin/env python3
"""Embed the offline management UI in the host server executable."""

from pathlib import Path
import sys

web = Path(__file__).resolve().parent
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

data = page.encode("utf-8")
lines = ["static const unsigned char admin_page[] = {"]
for pos in range(0, len(data), 32):
    lines.append("    " + ", ".join(str(byte) for byte in data[pos:pos + 32]) + ",")
lines.append("    0\n};\n")
Path(sys.argv[1]).write_text("\n".join(lines))

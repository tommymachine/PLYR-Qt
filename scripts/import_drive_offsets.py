#!/usr/bin/env python3
"""Import the AccurateRip drive-offset table into resources/drive_offsets.json.

Source: http://www.accuraterip.com/driveoffsets.htm (HTML table, FrontPage-
generated, alternating bgcolor rows, four <td>/<font> cells per row:
name, correction offset, submitter count, percentage agreement).

Drives marked [Purged] in the upstream are dropped (per AR docs, those have
no stable per-batch offset). Duplicate drive names are deduped to the row
with the highest submitter count. Names are canonicalized (uppercase,
whitespace collapsed) so the C++ lookup can do a straight string compare
on `(vendor + " " + product)` from IOKit.

Run with the upstream HTML on disk (saved via curl) or fetch directly.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.request
from pathlib import Path

AR_URL = "http://www.accuraterip.com/driveoffsets.htm"

# Four-cell row pattern. The HTML uses windows-1252 encoding and includes
# stray &nbsp; / non-ASCII bytes; we operate on the decoded string.
ROW_RE = re.compile(
    r"<tr>\s*"
    r"<td[^>]*>\s*<font[^>]*>([^<]*)</font>\s*</td>\s*"
    r"<td[^>]*>\s*<font[^>]*>([^<]+)</font>\s*</td>\s*"
    r"<td[^>]*>\s*<font[^>]*>(\d+)</font>\s*</td>\s*"
    r"<td[^>]*>\s*<font[^>]*>([^<]+)</font>\s*</td>\s*"
    r"</tr>",
    re.IGNORECASE | re.DOTALL,
)


def canonicalize(s: str) -> str:
    """Match CdDevice's canonicalize(): trim, collapse internal whitespace,
    uppercase. Internal spaces preserved (meaningful in drive names).

    AR's table uses " - " as the vendor/product separator (e.g.
    "PANASONIC - BD-CMB UJ-110"); we collapse that to a single space so
    a lookup key built as `canonicalize(vendor) + " " + canonicalize(product)`
    matches the AR entry. Internal "-" inside the product is preserved
    because there's no surrounding whitespace."""
    s = s.upper()
    s = s.replace(" - ", " ")
    return " ".join(s.split())


def parse(html: str) -> list[dict]:
    rows: dict[str, dict] = {}
    for m in ROW_RE.finditer(html):
        raw_name, raw_off, raw_subs, _agree = m.groups()
        name = canonicalize(raw_name)
        if not name:
            continue
        off_str = raw_off.strip()
        if off_str == "[Purged]":
            continue
        # Strip leading + the table uses (e.g. "+91"); int() handles it.
        try:
            offset = int(off_str)
        except ValueError:
            continue
        subs = int(raw_subs)
        prev = rows.get(name)
        if prev is None or subs > prev["submitters"]:
            rows[name] = {"name": name, "offset": offset, "submitters": subs}
    out = list(rows.values())
    out.sort(key=lambda r: r["name"])
    return out


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--from", dest="src", default=None,
                   help="HTML file on disk; otherwise fetched from AR.")
    p.add_argument("--out", default="resources/drive_offsets.json",
                   help="Output JSON path.")
    args = p.parse_args()

    if args.src:
        html = Path(args.src).read_text(encoding="windows-1252",
                                        errors="replace")
    else:
        with urllib.request.urlopen(AR_URL, timeout=30) as resp:
            html = resp.read().decode("windows-1252", errors="replace")

    entries = parse(html)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as f:
        json.dump(entries, f, ensure_ascii=False, indent=1)
        f.write("\n")

    print(f"wrote {len(entries)} entries to {out_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Cross-checks the --dry-run JSON against an independent Python reading of IPART.dbf.

Usage: python -I tests/check_dryrun.py <IPART.dbf> <dryrun.json>
Exits 1 on any mismatch. Uses only the standard library.
"""
import json
import struct
import sys
from decimal import Decimal, ROUND_HALF_UP


def read_dbf(path):
    d = open(path, "rb").read()
    n, hl, rl = struct.unpack("<IHH", d[4:12])
    fields, o, pos = [], 32, 1
    while d[o] != 0x0D:
        name = d[o:o + 11].split(b"\0")[0].decode("ascii")
        ftype, length = chr(d[o + 11]), d[o + 16]
        if ftype in "CN":  # binary fields (I, T, memo pointers) are not needed here
            fields.append((name, pos, length))
        pos += length
        o += 32
    rows = []
    for r in range(n):
        rec = d[hl + r * rl: hl + (r + 1) * rl]
        if rec[:1] == b"*":
            continue
        rows.append({f: rec[p:p + ln].decode("cp1252").strip() for f, p, ln in fields})
    return rows


def dec(s):
    if not s:
        return None
    return Decimal(s).quantize(Decimal("0.0001"), rounding=ROUND_HALF_UP).normalize()


def expected(rows):
    out, skipped = {}, []
    for r in rows:
        sku = r["IPPART"].upper()
        if r["IPCLAS"].upper() == "TEST" or sku.startswith("SHIP-"):
            skipped.append(sku)
            continue
        std, avg = dec(r["IPSMUC"]), dec(r["IPAMUC"])
        cost = std if std is not None and std > 0 else avg
        out[sku] = {
            "externalId": r["IPPART"],
            "sku": sku,
            "name": r["IPDESC"],
            "category": r["IPCLAS"] or None,
            "unitOfMeasure": {"EA": "each", "FT": "ft"}[r["IPIUM"].upper()],
            "standardCost": cost,
            "listPrice": dec(r["IPUPRC"]),
        }
    return out, skipped


def main():
    rows = read_dbf(sys.argv[1])
    exp, skipped = expected(rows)
    body = json.load(open(sys.argv[2], encoding="utf-8"), parse_float=Decimal, parse_int=Decimal)
    got = {i["sku"]: i for i in body["items"]}
    errors = []
    if body["source"] != "VISTA" or body["company"] != "SO":
        errors.append("bad source/company")
    if set(got) != set(exp):
        errors.append("SKU sets differ: missing %s extra %s" % (sorted(set(exp) - set(got)), sorted(set(got) - set(exp))))
    for sku, e in exp.items():
        g = got.get(sku)
        if g is None:
            continue
        for k, v in e.items():
            gv = g.get(k)
            if isinstance(v, Decimal) or isinstance(gv, Decimal):
                same = v is not None and gv is not None and Decimal(gv) == v
                same = same or (v is None and gv is None)
            else:
                same = gv == v
            if not same:
                errors.append("%s.%s: expected %r got %r" % (sku, k, v, gv))
    print("live records: %d, expected items: %d, got items: %d, skipped: %d (%s)"
          % (len(rows), len(exp), len(got), len(skipped), ", ".join(skipped)))
    for e in errors:
        print("MISMATCH", e)
    print("OK" if not errors else "FAILED")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

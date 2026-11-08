#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Ronny Hansen
"""Check the TAD asker's reply rule against every session in a RetroIO capture decode.

The rule in src/libxmsg/tad/tad_asker.h: for each host terminal frame the asker answers
    RESE -> RECO; only DUMM or only ESRS -> nothing; holds RFI -> nothing; holds CESC -> CERS;
    any other content -> DUMM.
For every session (host node, host port, asker port) the script predicts the sequence of control
replies (RECO, CERS, DUMM) from the host frames and compares it with what the asker really sent.
Order inside each machine's group of frames is the order sent, so the comparison needs no timing.

    python3 extras/tools/check_tad_reply_rule.py <pcap-decode-sample.json>

Result on RetroIO src/pcap-decode-sample.json (2026-11-08): 9 sessions MATCH; 1 DIFF whose host
frames are not in the file (0 predicted, 1 actual).
"""
import collections
import json
import sys


def opcodes(x):
    tad = x.get("Tad") or {}
    return [(m.get("OpcodeName") or hex(m["Opcode"])) for m in tad.get("Messages", [])]


def predict(ops):
    if "RESE" in ops:
        return "RECO"
    if ops in (["DUMM"], ["ESRS"]):
        return None
    if "RFI" in ops:
        return None
    if "CESC" in ops:
        return "CERS"
    return "DUMM"


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    with open(sys.argv[1], encoding="utf-8") as f:
        records = json.load(f)
    predicted = collections.defaultdict(list)
    actual = collections.defaultdict(list)
    for x in records:
        h = x["Header"]
        sh = x.get("SubHeader")
        if h["Subtype"] != "Data" or not sh:
            continue
        if (sh["ControlService"] >> 16) not in (0x0108, 0x0008):
            continue
        ops = opcodes(x)
        if sh["Role"] == 0:  # host frames carry role 0
            p = predict(ops)
            if p:
                predicted[(h["SourceNode"], sh["SourcePort"], sh["DestinationPort"])].append(p)
        elif len(ops) == 1 and ops[0] in ("DUMM", "CERS", "RECO"):
            actual[(h["DestinationNode"], sh["DestinationPort"], sh["SourcePort"])].append(ops[0])
    bad = 0
    for key in sorted(set(predicted) | set(actual)):
        a, b = predicted.get(key, []), actual.get(key, [])
        print(key, "MATCH" if a == b else "DIFF", len(a), len(b))
        if a != b:
            bad += 1
            print("  predicted", " ".join(a))
            print("  actual   ", " ".join(b))
    return 0 if bad == 0 else 2


if __name__ == "__main__":
    sys.exit(main())

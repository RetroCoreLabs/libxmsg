#!/usr/bin/env python3
"""Regenerate src/libxmsg/gen/*.h from the vendored protocol registry.

Two jobs:

  python extras/tools/regen.py
      run the vendored generate_c.py and write the headers into src/libxmsg/gen/.

  python extras/tools/regen.py --from <RetroIO>/src/DOC/protocols
      first refresh the vendored copy (the registry JSON files and generate_c.py) from that
      folder, record its git commit in extras/protocols/UPSTREAM.txt, then regenerate.

  python extras/tools/regen.py --check
      regenerate into a temporary folder and exit 1 if any header differs from the checked-in
      one. CI runs this so a hand edit of a generated file, or a vendored registry that moved
      without regenerating, fails the build.

The generator itself is owned by the registry (RetroIO / NDInsight); this script only drives it.
Paths are resolved relative to this file, never from the current directory.
"""

import argparse
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile
from datetime import date

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PROTOCOLS = os.path.join(REPO, "extras", "protocols")
GEN_OUT = os.path.join(REPO, "src", "libxmsg", "gen")

VENDORED_FILES = (
    "catalog.json",
    "sintran-wire.json",
    "tad-wire.json",
    "fa-qform.json",
    "xrout-services.json",
    "chat-wire.json",
    "generate_c.py",
)


def run_generator(out_dir):
    """Run the vendored generate_c.py into out_dir. Returns its exit code."""
    cmd = [sys.executable, "-I", os.path.join(PROTOCOLS, "generate_c.py"), "--out", out_dir]
    return subprocess.call(cmd)


def refresh_from(source_dir):
    """Copy the registry files from a RetroIO checkout and record its commit."""
    for name in VENDORED_FILES:
        src = os.path.join(source_dir, name)
        if not os.path.isfile(src):
            print("missing in source: %s" % src)
            return 1
        shutil.copyfile(src, os.path.join(PROTOCOLS, name))

    commit = "unknown"
    try:
        commit = subprocess.check_output(
            ["git", "-C", source_dir, "rev-parse", "HEAD"], universal_newlines=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        pass

    with open(os.path.join(PROTOCOLS, "UPSTREAM.txt"), "w", encoding="ascii", newline="\n") as f:
        f.write("Vendored copy of the XMSG protocol registry.\n\n")
        f.write("Source repository: RetroCoreLabs/RetroIO, folder src/DOC/protocols\n")
        f.write("Source commit:     %s\n" % commit)
        f.write("Copied:            %s\n\n" % date.today().isoformat())
        f.write("The source of truth is the public NDInsight repository, folder "
                "SINTRAN/XMSG/DOC/protocols;\n")
        f.write("RetroIO carries a copy and so does this repository. Do not edit the files here. "
                "To refresh:\n")
        f.write("  python extras/tools/regen.py --from <path to RetroIO>/src/DOC/protocols\n")
        f.write("which copies the registry, records the commit here and regenerates "
                "src/libxmsg/gen/*.h.\n")
    print("vendored registry refreshed from %s at %s" % (source_dir, commit))
    return 0


def check():
    """Regenerate into a temp folder and compare with the checked-in headers."""
    tmp = tempfile.mkdtemp(prefix="xmsg-gen-check-")
    try:
        if run_generator(tmp) != 0:
            return 1
        names = sorted(n for n in os.listdir(tmp) if n.endswith(".h"))
        checked_in = sorted(n for n in os.listdir(GEN_OUT) if n.endswith(".h"))
        if names != checked_in:
            print("generated file set differs: %s vs checked in %s" % (names, checked_in))
            return 1
        bad = [n for n in names if not filecmp.cmp(os.path.join(tmp, n), os.path.join(GEN_OUT, n),
                                                   shallow=False)]
        if bad:
            print("STALE generated headers (run extras/tools/regen.py): %s" % ", ".join(bad))
            return 1
        print("generated headers are current (%d files)" % len(names))
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from", dest="source", help="RetroIO src/DOC/protocols folder to vendor")
    parser.add_argument("--check", action="store_true", help="verify instead of writing")
    args = parser.parse_args()

    if args.source:
        rc = refresh_from(args.source)
        if rc != 0:
            return rc
    if args.check:
        return check()
    os.makedirs(GEN_OUT, exist_ok=True)
    return run_generator(GEN_OUT)


if __name__ == "__main__":
    sys.exit(main())

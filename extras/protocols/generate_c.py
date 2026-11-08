#!/usr/bin/env python3
"""Generate a C protocol library from the registries.

**A DRY RUN, like the C# one.** It writes to a temp folder so the question can be
answered before anything depends on it: do the registries carry what C needs?

C asks for things C# never did, which is exactly why it is worth generating:

  - fixed-width types, so a field's width must be stated rather than implied
  - explicit byte-swapping, because the wire is big-endian and the host usually
    is not - C# hid this behind a helper, C cannot
  - header guards and a flat namespace, so every name must be unique on its own
  - enums with a chosen underlying type, which C spells differently again

Anything the registry lacks shows up here as a generator that cannot emit, rather
than as a library that quietly does the wrong thing.

Usage:
    python generate_c.py                write to the temp folder and report
    python generate_c.py --out DIR      write somewhere specific
"""

import io
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

PROBLEMS = []


def comment(text, indent="", width=92):
    """A C block comment, wrapped."""
    if text is None:
        return []
    words = str(text).split()
    lines, current = [], ""
    for word in words:
        if len(current) + len(word) + 1 > width and current:
            lines.append(current)
            current = word
        else:
            current = (current + " " + word) if current else word
    if current:
        lines.append(current)

    if not lines:
        return []
    if len(lines) == 1:
        return ["%s/* %s */" % (indent, lines[0])]

    out = ["%s/*" % indent]
    for line in lines:
        out.append("%s * %s" % (indent, line))
    out.append("%s */" % indent)
    return out


def provenance(entry):
    status = entry.get("status")
    if not status:
        return None
    evidence = entry.get("evidence")
    return "%s: %s" % (status, evidence) if evidence else status


def c_name(prefix, name):
    """PREFIX_NAME, upper snake, which is what C constants look like.

    An ND symbol is ALREADY all capitals - XSLET, XRNSP - and splitting on every
    capital turns it into X_S_L_E_T. So the CamelCase split only applies to names
    that actually have lower case in them. C caught this; C# never could, because
    it keeps the member name verbatim.
    """
    text = str(name)
    if any(ch.islower() for ch in text):
        text = re.sub(r"(?<!^)(?=[A-Z])", "_", text)
    body = re.sub(r"[^A-Za-z0-9_]", "_", text).upper()
    return prefix + body


def gen_enum(block, fallback, out):
    binding = (block.get("bindings") or {}).get("c")
    if not binding:
        PROBLEMS.append("%s has no bindings.c - a C generator cannot name it" % fallback)
        return
    enum = binding.get("enum")
    prefix = binding.get("prefix")
    underlying = binding.get("underlying_type")
    if not enum or not prefix or not underlying:
        PROBLEMS.append("%s: bindings.c needs enum, prefix and underlying_type" % fallback)
        return

    entries = block.get("values") or block.get("bits") or []
    out += comment(block.get("note") or ("%s." % enum))
    out.append("typedef %s %s_t;" % (underlying, enum))
    out.append("")

    seen = {}
    for entry in entries:
        names = entry.get("names") or [entry.get("name")]
        value = entry.get("value")
        if value is None:
            value = entry.get("mask")
        for member in names:
            if member is None:
                continue
            symbol = c_name(prefix, member)
            if symbol in seen:
                # C has one flat namespace: two members that collapse to one symbol
                # would silently redefine each other.
                PROBLEMS.append("%s: %s and %s both become %s"
                                % (enum, seen[symbol], member, symbol))
                continue
            seen[symbol] = member

            remarks = []
            p = provenance(entry)
            if p:
                remarks.append(p)
            if entry.get("trap"):
                remarks.append("TRAP: " + entry["trap"])
            if entry.get("alias_of"):
                remarks.append("Same value as %s." % entry["alias_of"])
            text = entry.get("meaning") or member
            if remarks:
                text = text + "  --  " + "  ".join(remarks)
            out += comment(text)

            literal = value if isinstance(value, str) else str(value)
            out.append("#define %-34s %s" % (symbol, literal))
            out.append("")


def gen_struct(name, struct, out):
    """Accessors, not a packed struct.

    A packed struct depends on the host's endianness and on the compiler honouring
    a packing pragma. The wire is big-endian and defined to the byte, so the
    honest C is a function that reads the bytes.
    """
    fields = struct.get("fields", [])
    size = struct.get("size_bytes")
    out += comment("%s - %s bytes, %s on the wire. Accessors rather than a packed struct: a "
                   "packed struct depends on the host's byte order and on the compiler honouring "
                   "a pragma, while these read the bytes as defined."
                   % (name, size, struct.get("byte_order")))
    out.append("#define %s_SIZE %d" % (name.upper(), size))
    out.append("")

    for field in fields:
        bits = field.get("width_bits")
        offset = field.get("byte_offset")
        ctype = field.get("c_type")
        if bits is None or offset is None or ctype is None:
            PROBLEMS.append("%s.%s: needs width_bits, byte_offset and c_type for C"
                            % (name, field.get("name")))
            continue

        remarks = []
        p = provenance(field)
        if p:
            remarks.append(p)
        if field.get("rule"):
            remarks.append("Rule: " + field["rule"])
        text = field.get("meaning") or field["name"]
        if remarks:
            text = text + "  --  " + "  ".join(remarks)
        out += comment(text)

        fn = "%s_read_%s" % (name, field["name"])
        out.append("static inline %s %s(const uint8_t *frame)" % (ctype, fn))
        out.append("{")
        if bits == 16:
            out.append("    return (%s)(((uint16_t)frame[%d] << 8) | frame[%d]);"
                       % (ctype, offset, offset + 1))
        elif bits == 8:
            out.append("    return frame[%d];" % offset)
        elif bits == 32:
            out.append("    return ((uint32_t)frame[%d] << 24) | ((uint32_t)frame[%d] << 16)"
                       % (offset, offset + 1))
            out.append("         | ((uint32_t)frame[%d] << 8) | frame[%d];"
                       % (offset + 2, offset + 3))
        else:
            PROBLEMS.append("%s.%s: width %s has no C accessor" % (name, field["name"], bits))
            continue
        out.append("}")
        out.append("")


def main():
    out_dir = None
    if "--out" in sys.argv:
        out_dir = sys.argv[sys.argv.index("--out") + 1]
    if not out_dir:
        out_dir = os.path.join(tempfile.gettempdir(), "xmsg-generated-c")
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    catalog = json.load(io.open(os.path.join(HERE, "catalog.json"), encoding="utf-8"))
    written = []

    for entry in sorted(catalog["registries"], key=lambda e: e["read_order"]):
        doc = json.load(io.open(os.path.join(HERE, entry["file"]), encoding="utf-8"))
        stem = entry["file"].replace(".json", "").replace("-", "_")
        guard = "XMSG_%s_H" % stem.upper()

        out = []
        out.append("/* Generated from DOC/protocols/%s by generate_c.py. DO NOT EDIT. */"
                   % entry["file"])
        out.append("#ifndef %s" % guard)
        out.append("#define %s" % guard)
        out.append("")
        out.append("#include <stdint.h>")
        out.append("")
        out += comment(entry.get("one_line"))
        out.append("")

        for key in ("operations", "services", "errors", "connection_types",
                    "status_codes", "control_services"):
            block = doc.get(key)
            if block and block.get("values"):
                gen_enum(block, "%s/%s" % (entry["file"], key), out)

        for bf_name, bf in (doc.get("bitfields") or {}).items():
            gen_enum(bf, "%s/%s" % (entry["file"], bf_name), out)

        for st_name, st in (doc.get("structures") or {}).items():
            gen_struct(st_name, st, out)

        mp = doc.get("message_prefix")
        if mp and mp.get("fields"):
            # The registry names the C struct in bindings.c.struct (fa_message_prefix,
            # chat_message_prefix). Hard-coding "fa_message_prefix" here made two registries
            # emit the same symbol, so a program including both headers failed to compile
            # (found by libxmsg's smoke test, 2026-11-08). No binding is a registry gap, not a
            # reason to invent a name.
            mp_name = ((mp.get("bindings") or {}).get("c") or {}).get("struct")
            if not mp_name:
                PROBLEMS.append("%s/message_prefix has no bindings.c.struct - a C generator "
                                "cannot name it" % entry["file"])
            else:
                gen_struct(mp_name, dict(mp, size_bytes=mp.get("minimum_body_length", 0),
                                         byte_order="big-endian"), out)

        out.append("#endif /* %s */" % guard)
        path = os.path.join(out_dir, stem + ".h")
        io.open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
        written.append((entry["file"], stem + ".h", len(out)))

    print("generated into %s" % out_dir)
    for source, file, lines in written:
        print("  %-22s -> %-24s %4d lines" % (source, file, lines))

    if PROBLEMS:
        print("\nWHAT THE REGISTRY STILL LACKS FOR C:")
        for problem in sorted(set(PROBLEMS)):
            print("  " + problem)
        print("\n%d file(s), with gaps listed above. DRY RUN - nothing under SRC touched."
              % len(written))
        return 1

    print("\n%d file(s), no gaps. DRY RUN - nothing under SRC was touched." % len(written))
    return 0


if __name__ == "__main__":
    sys.exit(main())

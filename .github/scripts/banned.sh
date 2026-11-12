#!/bin/sh
# Fail when a banned function, or a host call the library must not make, appears in library code.
#
# House C standard rule 8.4 (banned string and conversion functions) and rules 7.4 / 9.4 (library
# code never exits, never prints, never allocates after start-up). Test code and examples are
# allowed those calls, so only src/ is scanned, and the generated headers are skipped.
set -eu

cd "$(dirname "$0")/../.."

files=$(find src -type f \( -name '*.c' -o -name '*.h' \) -not -path '*/gen/*' 2>/dev/null || true)
if [ -z "$files" ]; then
    echo "no library sources yet"
    exit 0
fi

pattern='\b(gets|strcpy|strcat|sprintf|vsprintf|strncpy|atoi|atol|atof|alloca|system|printf|fprintf|puts|perror|exit|abort|malloc|calloc|realloc|free|fopen|fread|fwrite|socket|recv|send)[[:space:]]*\('

# Strip comments and the text inside string literals first (a comment may name a banned function
# to say why it is not used, and a message such as "%u system(s)" is not a call), then grep what is
# left. strip_c.py keeps every newline, so the report points at the right line. The old sed only
# removed comments that opened and closed on one line, so every line inside a block comment was
# still searched.
status=0
for f in $files; do
    pat="$pattern"
    # libhdlc's ring buffer takes its storage from the heap when a buffer is CREATED
    # (ring_buffer_alloc / ring_buffer_alloc_internal: PSRAM or internal RAM on the ESP32) and
    # gives it back when it is destroyed (ring_buffer_free). House C standard rule 8.2 allows
    # exactly that: allocate at creation, free at destruction, never in the running path. The
    # firmware calls these at start-up (decided by Ronny, 2026-11-12). Only malloc and free are
    # let through, and only in this one file; every other banned call is still checked there.
    if [ "$f" = "src/libhdlc/ring_buffer.c" ]; then
        pat=$(printf '%s' "$pattern" | sed -e 's/|malloc|/|/' -e 's/|free|/|/')
    fi
    # Strip into a file first and stop on failure: in a pipe the helper's exit status is lost,
    # and a helper that prints nothing would let every file pass.
    if ! python3 .github/scripts/strip_c.py "$f" >/tmp/banned-src.$$; then
        echo "strip_c.py failed on $f"
        rm -f /tmp/banned-src.$$ /tmp/banned.$$
        exit 2
    fi
    if grep -nE "$pat" /tmp/banned-src.$$ >/tmp/banned.$$ 2>/dev/null; then
        echo "banned call in $f:"
        sed 's/^/    /' /tmp/banned.$$
        status=1
    fi
done
rm -f /tmp/banned.$$ /tmp/banned-src.$$

if [ "$status" -eq 0 ]; then
    echo "no banned calls in library code"
fi
exit "$status"

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

# Strip comments first (a comment may legitimately name a banned function to say why it is not
# used), then grep what is left. Lines are kept so the report points at the right place.
status=0
for f in $files; do
    if sed -e 's://.*$::' -e 's:/\*.*\*/::g' "$f" | grep -nE "$pattern" >/tmp/banned.$$ 2>/dev/null; then
        echo "banned call in $f:"
        sed 's/^/    /' /tmp/banned.$$
        status=1
    fi
done
rm -f /tmp/banned.$$

if [ "$status" -eq 0 ]; then
    echo "no banned calls in library code"
fi
exit "$status"

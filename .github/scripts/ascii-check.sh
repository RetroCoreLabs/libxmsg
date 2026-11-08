#!/bin/sh
# Sources are ASCII only, LF only, no tabs, no trailing whitespace, newline at end of file.
# House C standard rule 1.5. Applies to hand-written text files; binaries and the vendored
# registry (which the upstream project owns) are skipped.
set -eu

cd "$(dirname "$0")/../.."

files=$(git ls-files -- '*.c' '*.h' '*.txt' '*.md' '*.yml' '*.cmake' '*.py' '*.sh' '*.properties' \
            'CMakeLists.txt' '.clang-format' '.gitignore' '.gitattributes' \
        | grep -v '^extras/protocols/' || true)

status=0
for f in $files; do
    # Markdown is exempt from the ASCII rule: the house README template puts a middle dot between
    # the navigation links. Everything else must be plain ASCII.
    case "$f" in *.md) ascii_exempt=1 ;; *) ascii_exempt=0 ;; esac
    if [ "$ascii_exempt" -eq 0 ] && LC_ALL=C grep -nP '[^\x00-\x7F]' "$f" >/dev/null 2>&1; then
        echo "non-ASCII byte in $f:"
        LC_ALL=C grep -nP '[^\x00-\x7F]' "$f" | head -5 | sed 's/^/    /'
        status=1
    fi
    if grep -n $'\r' "$f" >/dev/null 2>&1; then
        echo "CRLF line ending in $f"
        status=1
    fi
    case "$f" in
        *.c|*.h|*.cmake|*.py|*.yml|*.md|CMakeLists.txt|*.properties)
            if grep -nP '\t' "$f" >/dev/null 2>&1; then
                echo "tab character in $f:"
                grep -nP '\t' "$f" | head -3 | sed 's/^/    /'
                status=1
            fi
            ;;
    esac
    if grep -nE '[[:space:]]+$' "$f" >/dev/null 2>&1; then
        echo "trailing whitespace in $f:"
        grep -nE '[[:space:]]+$' "$f" | head -3 | sed 's/^/    /'
        status=1
    fi
    if [ -s "$f" ] && [ "$(tail -c 1 "$f" | od -An -c | tr -d ' ')" != '\n' ]; then
        echo "no newline at end of $f"
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    echo "all text files are ASCII, LF, tab-free, with clean line ends"
fi
exit "$status"

"""Print a C file with its comments and the insides of its string and character literals blanked.

Used by banned.sh so that a comment or a message text that names a banned function ("abort (seven
ones)", "system(s)") is not reported as a call. Newlines are kept, so line numbers stay right.
usage: python3 strip_c.py <file.c>
"""
import sys


def strip(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if c == '/' and nxt == '/':
            while i < n and text[i] != '\n':
                i += 1
        elif c == '/' and nxt == '*':
            i += 2
            while i < n and not (text[i] == '*' and i + 1 < n and text[i + 1] == '/'):
                if text[i] == '\n':
                    out.append('\n')
                i += 1
            i += 2
            out.append(' ')
        elif c in '"\'':
            quote = c
            out.append(c)
            i += 1
            while i < n and text[i] != quote and text[i] != '\n':
                if text[i] == '\\':
                    i += 1
                i += 1
            out.append(quote)
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


sys.stdout.write(strip(open(sys.argv[1], encoding='ascii').read()))

# Contributing to libxmsg

Thanks for looking. This file says how to get a build running, what the code is expected to look
like, and what will get a change sent back.

libxmsg is a library with no main program, no sockets, no file system and no threads of its own.
Everything it needs from the outside world arrives through small tables of function pointers (the
"ports" in `src/libxmsg/xmsg_ports.h`), and every call that can start or expire a timer takes the
current time as a parameter. That is what lets one unit test be the transport and the clock at the
same time, and what lets the same `.c` files compile for a PC, an ESP32 and a Raspberry Pi Pico.

---

## Getting a build

| # | Requirement | How to check |
|---|---|---|
| 1 | CMake 3.13 or newer | `cmake --version` |
| 2 | A C11 compiler: GCC or Clang (MinGW GCC on Windows) | `gcc --version` |
| 3 | Python 3 | `python --version` - only for regenerating headers |

```bash
# from the repository root
cmake -S src -B build -DXMSG_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests labelled `integration` need a running nd100x emulator with its HDLC bridge on a TCP port
(`nd100x --hdlc=1:1362`). They read `ND100X_HDLC_HOST=127.0.0.1:1362` and report SKIPPED when it is
not set, so a plain `ctest` always runs to the end.

---

## What the code is expected to look like

The house C standard applies in full; the short version:

 - **C11 with GNU extensions, pinned in CMake.** Never rely on the compiler's default dialect.
 - **Zero warnings** with the warning set in `src/cmake/house_standard.cmake`. CI builds with
   `-Werror`; the fix for a warning is to fix the code, never to silence the flag.
 - **No allocation after start-up, no threads, no blocking calls** inside `src/`. Buffers are in the
   caller's structs or sized at compile time (`src/libxmsg/xmsg_config.h`).
 - **No `printf`, `exit`, `malloc` or any file or socket call** inside `src/`. Diagnostics go
   through the `XMSG_LOG` macro and the log port; everything else comes through a port.
 - **Banned functions**: `gets`, `strcpy`, `strcat`, `sprintf`, `vsprintf`, `strncpy`, `atoi`,
   `atol`, `atof`, `alloca`, `system`. CI greps for them.
 - **Fixed-width types** for anything that is a wire field or a buffer; `int` for counts and
   return codes. Functions that do something return `int`, 0 on success and negative on failure.
 - **Naming**: functions `module_verb` in snake case with the module as prefix (`lapb_rx_frame`),
   types `PascalCase` typedefs with no `_t`, constants `UPPER_SNAKE`, file-static state `s_name`.
 - **Formatting**: 4 spaces, Allman braces on everything including one-line bodies, 100 columns,
   `type *name`. `.clang-format` at the root is the authority; run it on the lines you changed.
 - **ASCII only**, LF line endings, no tabs, no trailing whitespace.
 - **Every non-static function has a Doxygen block in its header** saying what it does, what each
   parameter is, and what the return value means including the error convention.
 - **Keep the comments, and add more.** A comment that records a capture, a manual page, a
   measured behaviour of a real ND machine, or the reason a thing is NOT done the obvious way is
   the most valuable line in the file. Never delete one because a style guide prefers less;
   replace one only when it has become factually wrong.
 - **Plain words.** In code, comments, commit messages and documentation alike.

### Generated files

`src/libxmsg/gen/*.h` are generated from the protocol registry in `extras/protocols/` and must not
be edited. Change the registry upstream (RetroIO / NDInsight), refresh the vendored copy with
`python extras/tools/regen.py --from <RetroIO>/src/DOC/protocols`, and commit both. CI runs
`regen.py --check` and fails on any difference.

### Tests

 - **Unit tests only**, in `extras/tests/test_<module>.c`, one executable per module, registered
   with `xmsg_add_test` in `extras/tests/CMakeLists.txt`. No framework; `test_util.h` has the
   `CHECK`, `CHECK_EQ` and `CHECK_BYTES` macros and prints the verdict line CTest looks for.
 - **A wire-format change needs a test that reads real bytes**: a capture or a registry vector,
   never a hand-written buffer that agrees with the new code.
 - **Every layer has a peer-to-peer test**: two instances talk through the shared test transport
   in `extras/tests/pair/`, in memory with fault injection or over a loopback TCP socket.
 - Never add a standalone program that prints something and exits; it cannot fail a build.

---

## Documentation changes

> [!WARNING]
> **Never put an absolute path in anything committed here.** A drive letter or a home directory is
> correct on exactly one machine, and this repository is public. A path inside the repository goes
> repo-relative; a sibling repository is named, not located; anything outside is described.

The README follows the RetroCore Labs template: one sentence at the top that a stranger to the
subject understands, a code example early, install before build-from-source, licence last.

The documentation site (retrocorelabs.github.io/libxmsg) is written from the headers: the
comment block at the top of each header and the `/** ... */` block before each declaration
become its reference page. To change a page, change the comment in the header. Nothing
generated is committed; `.github/workflows/docs.yml` builds and publishes it on every push to
main. To build it yourself, from the repository root, with Python 3.11 or newer:

```sh
pip install -r extras/docs-site/requirements.txt
python3 extras/tools/gen_docs.py build-site
mkdocs build --strict -f build-site/mkdocs.yml -d <a folder outside the checkout>
```

A warning fails the build: a link that goes nowhere is a dead link on the live site.

---

## Submitting a change

 1. Branch off `main`. Do not commit to `main` directly.
 2. One logical change per commit. A commit message says **what changed and why**, in plain words -
    the subject line in the imperative, then a blank line, then the reasoning. If you measured
    something, put the number in the message.
 3. The build and `ctest` pass before you open the pull request.
 4. Say in the pull request what you verified and how. "Tests pass" on its own is not useful; "the
    LAPB conformance vectors S1-S7 are byte-exact, 41 checks, and the link stayed up 2 minutes
    against nd100x" is.
 5. If something is still broken or unfinished, say so plainly. A known gap that is written down is
    fine. One that is hidden is not.

---

## Licence

By contributing you agree your work is licensed under the MIT licence in [LICENSE](../LICENSE).

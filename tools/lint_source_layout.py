#!/usr/bin/env python3
"""lint_source_layout.py — keep piet/native/src organised by function.

WHY THIS EXISTS
---------------
The native sources were reorganised so that a maintainer (human or agent) can go
from a symptom to one directory and one file: sources live in domain directories,
every file is named for what it does, and each directory's README maps its files.
None of that is enforced by the compiler. A new source dropped into the wrong
place, a file left out of the build list, a stale path in the floating-point flag
list (CMake does not diagnose it, and the file silently recompiles with fused
multiply-add) or a README that no longer lists a file all build and pass tests.
This lint is the check that fails instead.

RULES (each has a seeded failing case in --self-test)
  R1 root      no C++ file directly in piet/native/src except selftest*
  R2 build     every .cpp/.mm under src is in exactly one build list of
               piet/native/CMakeLists.txt, and every listed path exists
  R3 fpflags   every path in the -ffp-contract=off block exists and is built
               (this catches a stale path; it cannot know that a NEW piece cut
               from a flagged file belongs on the list — whoever splits a
               flagged file adds every piece, and says so in the README)
  R4 unique    code-file basenames are unique across src (grep and compiler
               diagnostics name a file by its basename)
  R5 names     no development-history token in a file name, no reserved
               directory name that would shadow the engine's include paths
  R6 readme    every directory under src has a README.md that names each of
               its code files, and names no file that does not exist
  R7 includes  no '../' in a quoted include under src

usage: tools/lint_source_layout.py [--root DIR] [--self-test]
exit 0 = clean, 1 = violations (one line each), 2 = usage/setup error
"""
import collections
import os
import re
import shutil
import sys
import tempfile

CODE_EXT = (".cpp", ".h", ".hpp", ".inc", ".mm")
BUILT_EXT = (".cpp", ".mm")
# Tokens that name a development wave or lane rather than a function.
HISTORY = re.compile(r"(?:^|_)(aer|e3|t1|t7|qe|k1|lb|m3|m4|r150|w2[a-z]{0,2})(?:_|\.|$)")
# build/dist are ignored by git at any depth. The engine's own directory names (repo-root src/*) are
# added at run time: a PIET directory of the same name would sit ahead of them on the include path.
RESERVED_DIRS = {"build", "dist"}
# Directories whose contents are not hand-maintained sources.
SKIP_DIRS = {"generated"}
ROW = re.compile(r"(?<![\w/}])src/([\w./-]+\.(?:cpp|mm))\b")


def code_files(src):
    out = []
    for d, dn, fn in os.walk(src):
        dn[:] = sorted(x for x in dn if x != "build")
        for f in sorted(fn):
            if f.endswith(CODE_EXT):
                out.append(os.path.relpath(os.path.join(d, f), src).replace(os.sep, "/"))
    return out


def cmake_lists(cmake_path):
    """Return (build_rows Counter, fp_rows list) of src-relative paths."""
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/x.cpp names the same file as src/x.cpp
    lines = open(cmake_path, encoding="utf-8").read().replace("${CMAKE_CURRENT_SOURCE_DIR}/", "").split("\n")
    fp_rows, build = [], collections.Counter()
    i = 0
    while i < len(lines):
        if lines[i].lstrip().startswith("set_source_files_properties("):
            j = i
            while j < len(lines) and ")" not in lines[j].split("#")[0]:
                j += 1
            block = lines[i:j + 1]
            rows = [m.group(1) for l in block for m in ROW.finditer(l.split("#")[0])]
            if any("ffp-contract=off" in l for l in block):
                fp_rows += rows
            i = j + 1
            continue
        for m in ROW.finditer(lines[i].split("#")[0]):
            build[m.group(1)] += 1
        i += 1
    return build, fp_rows


def lint(root):
    native = os.path.join(root, "piet", "native")
    src = os.path.join(native, "src")
    cmake = os.path.join(native, "CMakeLists.txt")
    if not os.path.isdir(src) or not os.path.isfile(cmake):
        print(f"lint_source_layout: no piet/native tree under {root}", file=sys.stderr)
        return None
    bad = []
    files = code_files(src)
    hand = [f for f in files if f.split("/")[0] not in SKIP_DIRS]

    # R1
    for f in hand:
        if "/" not in f and not f.startswith("selftest"):
            bad.append(f"R1 root: {f} sits directly in piet/native/src; move it to its domain directory")

    # R2 / R3
    build, fp_rows = cmake_lists(cmake)
    on_disk = set(f for f in files if f.endswith(BUILT_EXT))
    for f in sorted(on_disk):
        if build.get(f, 0) == 0:
            bad.append(f"R2 build: {f} is in no build list of piet/native/CMakeLists.txt")
        elif build[f] > 1:
            bad.append(f"R2 build: {f} is listed {build[f]} times in the build lists")
    for f in sorted(build):
        if f not in on_disk:
            bad.append(f"R2 build: CMakeLists.txt lists src/{f}, which does not exist")
    for f in fp_rows:
        if f not in on_disk:
            bad.append(f"R3 fpflags: the -ffp-contract=off list names src/{f}, which does not exist "
                       "(the flag is silently lost)")
    for f, n in collections.Counter(fp_rows).items():
        if n > 1:
            bad.append(f"R3 fpflags: src/{f} appears {n} times in the -ffp-contract=off list")

    # R4
    by_base = collections.defaultdict(list)
    for f in hand:
        by_base[os.path.basename(f)].append(f)
    for b, fs in sorted(by_base.items()):
        if len(fs) > 1:
            bad.append(f"R4 unique: basename {b} is used by {', '.join(fs)}")

    # R5
    for f in hand:
        b = os.path.basename(f)
        m = HISTORY.search(b)
        if m:
            bad.append(f"R5 names: {f} carries the development-history token '{m.group(1)}'; name it for its function")
    engine_src = os.path.join(root, "src")
    reserved = set(RESERVED_DIRS)
    if os.path.isdir(engine_src):
        reserved.update(x for x in os.listdir(engine_src) if os.path.isdir(os.path.join(engine_src, x)))
    for d in sorted(set(f.split("/")[0] for f in files if "/" in f)):
        if d in reserved:
            bad.append(f"R5 names: directory src/{d} shadows an engine include path or is git-ignored")

    # R6
    everywhere = set(os.path.basename(f) for f in files)
    inc_dir = os.path.join(native, "include")
    for d, dn, fn in os.walk(inc_dir):
        everywhere.update(fn)
    dirs = collections.defaultdict(list)
    for f in hand:
        if "/" in f:
            dirs[f.split("/")[0]].append(f)
    for d, fs in sorted(dirs.items()):
        readme = os.path.join(src, d, "README.md")
        if not os.path.isfile(readme):
            bad.append(f"R6 readme: src/{d}/ has no README.md mapping its files")
            continue
        text = open(readme, encoding="utf-8").read()
        named = set(re.findall(r"`([\w./-]+\.(?:cpp|h|hpp|inc|mm))`", text))
        named_base = set(os.path.basename(n) for n in named)
        here = set(os.path.basename(f) for f in fs)
        for b in sorted(here - named_base):
            bad.append(f"R6 readme: src/{d}/README.md does not name {b}")
        for n in sorted(named):
            # a README may point at files elsewhere; a bare name must exist somewhere in the native tree
            if "/" not in n and n not in here and n not in everywhere:
                bad.append(f"R6 readme: src/{d}/README.md names {n}, which does not exist under piet/native")
            elif "/" in n:
                cand = [os.path.normpath(os.path.join(src, d, n)), os.path.join(src, n),
                        os.path.join(native, n), os.path.join(native, "include", n), os.path.join(root, n)]
                if not any(os.path.isfile(c) for c in cand):
                    bad.append(f"R6 readme: src/{d}/README.md names {n}, which does not exist")

    # R7
    inc = re.compile(r'^\s*#\s*include\s*"(\.\./[^"]*)"')
    for f in hand:
        with open(os.path.join(src, f), encoding="utf-8", errors="replace") as fh:
            for n, line in enumerate(fh, 1):
                m = inc.match(line)
                if m:
                    bad.append(f"R7 includes: {f}:{n} includes \"{m.group(1)}\"; use the src-root-relative path")
    return bad


def self_test():
    """Build a small clean tree, then seed one violation per rule and require each to be reported."""
    tmp = tempfile.mkdtemp(prefix="layout_lint_")
    try:
        def fresh():
            shutil.rmtree(tmp)
            src = os.path.join(tmp, "piet", "native", "src")
            os.makedirs(os.path.join(src, "tables"))
            os.makedirs(os.path.join(src, "graph"))
            os.makedirs(os.path.join(tmp, "src", "io"))      # an engine directory name
            w = lambda p, t: open(os.path.join(tmp, p), "w", encoding="utf-8").write(t)
            w("piet/native/src/selftest.cpp", "int main(){}\n")
            w("piet/native/src/tables/table_render.cpp", '#include "tables_internal.h"\n')
            w("piet/native/src/tables/tables_internal.h", "\n")
            w("piet/native/src/graph/svg.cpp", '#include "tables/tables_internal.h"\n')
            w("piet/native/src/tables/README.md", "| x | `table_render.cpp` |\n`tables_internal.h`\n")
            w("piet/native/src/graph/README.md", "`svg.cpp`, see `tables/tables_internal.h`\n")
            w("piet/native/CMakeLists.txt",
              "set_source_files_properties(\n  src/graph/svg.cpp\n  PROPERTIES\n  COMPILE_FLAGS \"-ffp-contract=off\"\n)\n"
              "add_library(piet_core OBJECT\n  src/tables/table_render.cpp\n  src/graph/svg.cpp\n)\n"
              "add_executable(piet_selftest src/selftest.cpp\n  $<TARGET_OBJECTS:piet_core>)\n")
            return w
        w = fresh()
        base = lint(tmp)
        ok = base == []
        if not ok:
            print("self-test: the clean tree is not clean:", base)
        seeds = []
        def seed(rule, fn):
            w = fresh(); fn(w); got = lint(tmp)
            hit = any(g.startswith(rule) for g in got)
            seeds.append((rule, hit))
            if not hit:
                print(f"self-test: seeded {rule} violation was NOT reported; got {got}")
        seed("R1", lambda w: (w("piet/native/src/stray.cpp", "\n"),
                              open(os.path.join(tmp, "piet/native/CMakeLists.txt"), "a").write("add_library(x OBJECT\n  src/stray.cpp\n)\n")))
        seed("R2", lambda w: w("piet/native/src/tables/table_new.cpp", "\n"))
        seed("R2", lambda w: os.remove(os.path.join(tmp, "piet/native/src/tables/table_render.cpp")))
        seed("R3", lambda w: w("piet/native/CMakeLists.txt",
                                open(os.path.join(tmp, "piet/native/CMakeLists.txt")).read().replace("  src/graph/svg.cpp\n  PROPERTIES", "  src/svg.cpp\n  PROPERTIES")))
        cm = lambda: open(os.path.join(tmp, "piet/native/CMakeLists.txt")).read()
        seed("R2", lambda w: w("piet/native/CMakeLists.txt", cm().replace("  src/tables/table_render.cpp\n", "  src/tables/table_render.cpp\n  src/tables/table_render.cpp\n")))
        seed("R3", lambda w: w("piet/native/CMakeLists.txt", cm().replace("  src/graph/svg.cpp\n  PROPERTIES", "  src/graph/svg.cpp\n  ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/svg.cpp\n  PROPERTIES")))
        seed("R3", lambda w: w("piet/native/CMakeLists.txt", cm().replace("  src/graph/svg.cpp\n  PROPERTIES", "  ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/gone.cpp\n  PROPERTIES")))
        seed("R6", lambda w: w("piet/native/src/graph/README.md", "`svg.cpp`, see `tables/gone.h`\n"))
        seed("R4", lambda w: (w("piet/native/src/graph/tables_internal.h", "\n"),))
        seed("R5", lambda w: (os.rename(os.path.join(tmp, "piet/native/src/graph/svg.cpp"), os.path.join(tmp, "piet/native/src/graph/handlers_aer_svg.cpp")),))
        seed("R5", lambda w: (os.makedirs(os.path.join(tmp, "piet/native/src/io")), w("piet/native/src/io/x.h", "\n"), w("piet/native/src/io/README.md", "`x.h`\n")))
        seed("R6", lambda w: w("piet/native/src/tables/README.md", "`table_render.cpp`\n"))
        seed("R6", lambda w: w("piet/native/src/graph/README.md", "`svg.cpp` `gone.cpp`\n"))
        seed("R6", lambda w: os.remove(os.path.join(tmp, "piet/native/src/graph/README.md")))
        seed("R7", lambda w: w("piet/native/src/graph/svg.cpp", '#include "../tables/tables_internal.h"\n'))
        ok = ok and all(h for _, h in seeds)
        print(f"lint_source_layout --self-test: {'PASS' if ok else 'FAIL'} "
              f"(clean tree clean; {sum(h for _, h in seeds)}/{len(seeds)} seeded violations reported)")
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv):
    if "--self-test" in argv:
        return self_test()
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    if "--root" in argv:
        root = argv[argv.index("--root") + 1]
    bad = lint(root)
    if bad is None:
        return 2
    for b in bad:
        print(b)
    print(f"[layout] {'GREEN' if not bad else 'RED'} — {len(bad)} violation(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

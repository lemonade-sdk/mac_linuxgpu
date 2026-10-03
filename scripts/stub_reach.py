#!/usr/bin/env python3
"""Over-approximate which generated kernel API stubs upstream code can reach.

The graph is built by grepping source text, not by compiling it:

  * every top-level function definition in the built driver sources, in
    linuxu/src (minus the generated stub file) and every static inline
    function in the shadow/driver headers becomes a node;
  * every top-level initialised object (ops tables, IP block descriptors,
    drm_driver, ...) becomes a node too, so indirect calls through ops
    tables are followed from the code that names the table;
  * every function-like or object-like #define becomes a node;
  * an edge goes from a node to every identifier named in its body.

Both sides of every #if are kept and static functions are merged by name,
so the result is a superset of the real call graph.  Anything reported as
unreachable here is genuinely unreachable from the given roots by direct
reference; anything reported reachable may or may not run.

Usage:
  stub_reach.py [--roots a,b,c] [--stubs FILE] [--paths]

With no --stubs, the stub names are read from the generated
linuxu/src/shims/kernel_api_stubs.c.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upstream_tree  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GENERATED = os.path.join(ROOT, "linuxu", "src", "shims", "kernel_api_stubs.c")
DEFAULT_ROOTS = ("amdgpu_device_init", "amdgpu_driver_open_kms",
                 "kfd_create_process")

IDENT = re.compile(r"[A-Za-z_]\w*")
C_KEYWORDS = frozenset("""
auto break case char const continue default do double else enum extern float
for goto if inline int long register restrict return short signed sizeof
static struct switch typedef union unsigned void volatile while bool true false
NULL typeof __typeof__ __attribute__ asm __asm__ _Bool
""".split())


def built_driver_sources():
    return sorted(upstream_tree.built_driver_sources())


def walk(base, suffix, skip=()):
    for dirpath, _, files in os.walk(base):
        for f in files:
            p = os.path.join(dirpath, f)
            if f.endswith(suffix) and p not in skip:
                yield p


_STRIP = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
    re.S)


def strip_comments_and_strings(text):
    def repl(m):
        s = m.group(0)
        if s.startswith("/*"):
            return "\n" * s.count("\n")
        if s.startswith("//"):
            return ""
        return '""'
    return _STRIP.sub(repl, text)


def split_preprocessor(text):
    """Return (code_without_directives, {macro_name: body})."""
    macros = {}
    out = []
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.lstrip().startswith("#"):
            full = line
            while full.endswith("\\") and i + 1 < len(lines):
                i += 1
                full = full[:-1] + " " + lines[i]
            m = re.match(r"\s*#\s*define\s+(\w+)(\([^)]*\))?(.*)", full)
            if m:
                macros.setdefault(m.group(1), []).append(m.group(3))
            out.append("")
        else:
            out.append(line)
        i += 1
    return "\n".join(out), macros


_FUNC_HEAD = re.compile(r"(\w+)\s*\((?:[^()]|\([^()]*\))*\)\s*"
                        r"(?:__\w+(?:\s*\((?:[^()]|\([^()]*\))*\))?\s*)*$", re.S)
_DATA_HEAD = re.compile(r"(\w+)\s*(?:\[[^\]]*\]\s*)*=\s*$", re.S)


def top_level_definitions(code):
    """Yield (name, body) for top-level functions and initialised objects."""
    depth = 0
    head_start = 0
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c == "{" and depth == 0:
            head = code[head_start:i]
            j = i + 1
            d = 1
            while j < n and d:
                if code[j] == "{":
                    d += 1
                elif code[j] == "}":
                    d -= 1
                j += 1
            body = code[i + 1:j - 1]
            fm = _FUNC_HEAD.search(head)
            dm = _DATA_HEAD.search(head)
            if dm:
                yield dm.group(1), body
            elif fm and "=" not in head[fm.start():]:
                name = fm.group(1)
                if name not in C_KEYWORDS:
                    yield name, body
            i = j
            head_start = j
            continue
        if c in ";}" and depth == 0:
            head_start = i + 1
        elif c == "=" and depth == 0:
            pass
        i += 1


def build_graph(paths):
    graph = collections.defaultdict(set)
    for p in paths:
        try:
            text = open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        code, macros = split_preprocessor(strip_comments_and_strings(text))
        for name, bodies in macros.items():
            for body in bodies:
                graph[name].update(IDENT.findall(body))
        for name, body in top_level_definitions(code):
            graph[name].update(IDENT.findall(body))
    for k in graph:
        graph[k] -= C_KEYWORDS
    return graph


def generated_stub_names(path=GENERATED):
    names = set()
    text = strip_comments_and_strings(open(path).read())
    code, _ = split_preprocessor(text)
    for name, _body in top_level_definitions(code):
        names.add(name)
    return names


def reach(graph, root, stubs):
    parent = {root: None}
    q = collections.deque([root])
    hits = []
    while q:
        cur = q.popleft()
        if cur in stubs and cur != root:
            hits.append(cur)
            continue  # stubs are leaves
        for nxt in graph.get(cur, ()):
            if nxt not in parent and (nxt in graph or nxt in stubs):
                parent[nxt] = cur
                q.append(nxt)
    paths = {}
    for h in hits:
        chain = [h]
        while parent[chain[-1]] is not None:
            chain.append(parent[chain[-1]])
        paths[h] = list(reversed(chain))
    return sorted(hits), paths, len(parent)


def all_sources():
    skip = {GENERATED}
    paths = built_driver_sources()
    paths += upstream_tree.helper_sources()
    paths += list(walk(os.path.join(ROOT, "linuxu", "src"), ".c", skip))
    paths += list(walk(os.path.join(ROOT, "linuxu", "headers"), ".h"))
    # Driver-local headers, without the disabled ACP and display trees.
    paths += [p for p in upstream_tree.driver_header_files()
              if "/amd/acp/" not in p and "/amd/display/" not in p]
    return paths


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", default=",".join(DEFAULT_ROOTS))
    ap.add_argument("--stubs", help="file with one stub name per line")
    ap.add_argument("--paths", action="store_true",
                    help="print one example reference chain per stub")
    args = ap.parse_args()
    if args.stubs:
        stubs = {l.strip() for l in open(args.stubs) if l.strip()}
    else:
        stubs = generated_stub_names()
    graph = build_graph(all_sources())
    # The stub file is excluded from the graph, so a stub never has
    # outgoing edges here even when a real definition shares its name.
    for s in stubs:
        graph.pop(s, None)
    for root in args.roots.split(","):
        if root not in graph:
            sys.exit(f"stub_reach: root {root} not found in upstream sources")
        hits, paths, visited = reach(graph, root, stubs)
        print(f"root {root}: {visited} nodes visited, {len(hits)} stubs reachable")
        for h in hits:
            if args.paths:
                print(f"  {h}: " + " -> ".join(paths[h]))
            else:
                print(f"  {h}")


if __name__ == "__main__":
    main()

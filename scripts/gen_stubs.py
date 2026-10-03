#!/usr/bin/env python3
"""Generate linuxu/src/shims/kernel_api_stubs.c from a fail-safe policy.

Every symbol in linuxu/UNDEFINED_SYMBOLS.txt (the link-undefined kernel,
DRM and libc surface of the DriverKit image) must be classified in
scripts/stub_policy.py. The generator refuses to run (exit status 1) when:

  * a symbol is not classified, or the policy names a symbol that is not in
    UNDEFINED_SYMBOLS.txt;
  * a symbol is classified `implement` (it needs a real implementation and
    must never be satisfied by a generated body);
  * an `external` symbol has no definition in the non-generated sources, or
    a generated symbol also has a definition there (duplicate at link time);
  * a body would hand a negative errno to a caller through an unsigned,
    enum, bool or pointer return type, or a category does not fit the
    return type (for example `noop` on a non-void function);
  * no declaration (or `proto` override) gives the exact signature.

Categories (see scripts/stub_policy.py for the per-symbol reasons):

  external         defined by a hand-written shim, a DriverKit adapter or a
                   pinned upstream file; not generated.
  disabled-inline  body copied from the static inline that the pinned
                   third_party/linux/include header provides for our !CONFIG
                   configuration (`header` names it).
  ok-zero          0 / NULL / false is the correct Linux result here.
  noop             void function whose effect does not exist in this
                   environment.
  unreachable      not expected to run in the supported configuration:
                   one-shot warning naming the symbol, then a benign value
                   (0 / NULL / false, or `ret`).
  enosys           explicit user-facing ioctl/feature that is unsupported;
                   returns -ENOSYS (signed integer returns only).
  data             object definition given verbatim by `def`.
  implement        must not be generated: the generator fails.

Usage:
  python3 scripts/gen_stubs.py            regenerate the stub file
  python3 scripts/gen_stubs.py --check    verify the policy and that the
                                          checked-in file is up to date
  --symbols FILE / --policy MODULE / --output FILE override the inputs
  (used by scripts/test-stub-policy.sh).
"""
import argparse
import importlib.util
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upstream_tree  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMS_FILE = os.path.join(ROOT, "linuxu", "UNDEFINED_SYMBOLS.txt")
POLICY_FILE = os.path.join(ROOT, "scripts", "stub_policy.py")
OUT_FILE = os.path.join(ROOT, "linuxu", "src", "shims", "kernel_api_stubs.c")
SHADOW = os.path.join(ROOT, "linuxu", "headers")
VENDOR_INCLUDE = upstream_tree.LINUX_INCLUDE
# Declaration search order: shadow headers first, then the driver-local
# upstream headers.
DECL_SKIP = ("/asic_reg/", "/tests/", "/display/dc/")

CATEGORIES = ("external", "disabled-inline", "ok-zero", "noop",
              "unreachable", "enosys", "data", "implement")
GENERATED = ("disabled-inline", "ok-zero", "noop", "unreachable", "enosys",
             "data")


class PolicyError(Exception):
    pass


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------
def load_policy(path):
    spec = importlib.util.spec_from_file_location("stub_policy", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    policy = {}
    for sym, entry in mod.POLICY.items():
        if isinstance(entry, str):
            entry = {"cat": entry}
        elif isinstance(entry, tuple):
            entry = {"cat": entry[0], "why": entry[1]}
        else:
            entry = dict(entry)
        policy[sym] = entry
    return policy, getattr(mod, "STUB_INCLUDES", [])


def read_symbols(path):
    with open(path) as f:
        syms = [line.strip() for line in f if line.strip()]
    dups = sorted({s for s in syms if syms.count(s) > 1})
    if dups:
        raise PolicyError("duplicate entries in %s: %s" % (path, " ".join(dups)))
    return syms


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"),
                  text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


# ---------------------------------------------------------------------------
# Definitions outside the generated file
# ---------------------------------------------------------------------------
def definition_sources(output):
    out = upstream_tree.built_driver_sources() + upstream_tree.helper_sources()
    for base, exts in ((os.path.join(ROOT, "linuxu", "src"), (".c",)),
                       (os.path.join(ROOT, "dext", "sources"),
                        (".c", ".m", ".mm", ".cpp"))):
        for dirpath, _, files in os.walk(base):
            out += [os.path.join(dirpath, f) for f in files if f.endswith(exts)]
    out = [p for p in out if os.path.abspath(p) != os.path.abspath(output)]
    # The generated file of the real tree is never a definition source, even
    # when a test writes its output elsewhere.
    return [p for p in out if os.path.abspath(p) != os.path.abspath(OUT_FILE)]


def find_definitions(symbols, output):
    """Map symbol -> [relative paths] that define it (text heuristic)."""
    wanted = set(symbols)
    found = {s: [] for s in symbols}
    word = re.compile(r"[A-Za-z_]\w*")
    for path in definition_sources(output):
        try:
            raw = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        hits = wanted.intersection(word.findall(raw))
        if not hits:
            continue
        text = strip_comments(raw)
        for sym in hits:
            s = re.escape(sym)
            # Definitions start in column 0 (kernel style); indented text
            # is a call or a statement.
            func = re.compile(r"(?m)^(?!(?:extern\b(?!\s*\"C\")|typedef|return\b))"
                              r"(?:[A-Za-z_][^\n;{}()=]*)?\b" + s +
                              r"\s*\(([^;{}]|\n)*?\)"
                              r"[\s\w()]*\{")
            data = re.compile(r"(?m)^(?!(?:extern\b|typedef|return\b))"
                              r"[A-Za-z_][^\n;{}()=]*[\s*]" + s +
                              r"\s*(?:\[[^\]]*\]\s*)*(?:=|;)")
            if func.search(text) or data.search(text):
                found[sym].append(os.path.relpath(path, ROOT))
    return found


# ---------------------------------------------------------------------------
# Signatures
# ---------------------------------------------------------------------------
_HEADER_CACHE = {}


def header_texts():
    if not _HEADER_CACHE:
        paths = []
        for dirpath, _, files in os.walk(SHADOW):
            if any(k in dirpath + "/" for k in DECL_SKIP):
                continue
            paths += [os.path.join(dirpath, f) for f in files if f.endswith(".h")]
        paths += upstream_tree.driver_header_files(DECL_SKIP)
        for p in paths:
            try:
                t = open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            _HEADER_CACHE[p] = t
    return _HEADER_CACHE


def _balanced(text, start):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def find_prototype(sym):
    """Return (return_type, params, header_path) of a declaration of sym."""
    for path, raw in header_texts().items():
        if sym not in raw:
            continue
        text = strip_comments(raw)
        # Join continuation-free lines; drop preprocessor lines.
        text = "\n".join("" if l.lstrip().startswith("#") else l
                         for l in text.split("\n"))
        for m in re.finditer(r"\b" + re.escape(sym) + r"\s*\(", text):
            head_start = max(text.rfind(";", 0, m.start()),
                             text.rfind("}", 0, m.start()),
                             text.rfind("{", 0, m.start())) + 1
            head = " ".join(text[head_start:m.start()].split())
            if not head or "static" in head.split() or "=" in head or \
                    "(" in head or "return" in head.split():
                continue
            pend = _balanced(text, m.end() - 1)
            if pend < 0:
                continue
            tail = text[pend + 1:pend + 200].lstrip()
            if not tail.startswith(";") and not re.match(
                    r"(__\w+(\s*\([^;]*?\))?\s*)+;", tail):
                continue
            rt = re.sub(r"\b(extern|inline|__must_check|__cold|asmlinkage|"
                        r"__printf\s*\([^)]*\)|__malloc|__init|"
                        r"__attribute__\s*\(\(.*?\)\))\s*", "", head).strip()
            if not rt or not re.match(r"^[A-Za-z_][\w\s\*]*$", rt):
                continue
            params = " ".join(text[m.end():pend].split())
            return rt, params or "void", path
    return None


def return_kind(rt):
    t = " ".join(rt.replace("const ", "").split())
    if t == "void":
        return "void"
    if "*" in t:
        return "pointer"
    if t in ("bool", "_Bool"):
        return "bool"
    if t.startswith("enum ") or t in ("irqreturn_t",):
        return "enum"
    if re.match(r"^(unsigned\b|u8|u16|u32|u64|uint\d+_t|size_t|acpi_status|"
                r"dma_addr_t|phys_addr_t|gfp_t|umode_t|dev_t|sector_t)", t):
        return "unsigned"
    if re.match(r"^(int|long|signed|short|char|s8|s16|s32|s64|int\d+_t|"
                r"ssize_t|loff_t|ktime_t|pid_t|blk_status_t)\b", t):
        return "signed"
    if t.startswith(("struct ", "union ")):
        return "aggregate"
    return "unknown"


def benign_value(kind, rt):
    if kind == "pointer":
        return "NULL"
    if kind == "bool":
        return "false"
    if kind == "aggregate":
        return "(" + rt + "){0}"
    return "0"


def is_negative(expr):
    e = expr.strip()
    while e.startswith("("):
        e = e[1:].lstrip()
    return e.startswith("-") or bool(re.search(r"ERR_PTR\s*\(\s*-", e))


# ---------------------------------------------------------------------------
# disabled-inline: copy the vendor !CONFIG static inline
# ---------------------------------------------------------------------------
def vendor_inline(sym, header):
    path = os.path.join(VENDOR_INCLUDE, header)
    text = strip_comments(open(path).read())
    pat = re.compile(r"static\s+(?:__always_)?inline\s+([^;{}()]*?)\b" +
                     re.escape(sym) + r"\s*\(")
    hits = list(pat.finditer(text))
    if len(hits) != 1:
        raise PolicyError("%s: expected exactly one static inline in "
                          "third_party/linux/include/%s, found %d"
                          % (sym, header, len(hits)))
    m = hits[0]
    pend = _balanced(text, m.end() - 1)
    params = " ".join(text[m.end():pend].split())
    bstart = text.index("{", pend)
    depth, i = 0, bstart
    while True:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    body = text[bstart + 1:i].strip("\n")
    rt = " ".join(m.group(1).split())
    return rt, params or "void", body


# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------
HEADER = """\
/* linuxu shim: kernel_api_stubs - policy-classified kernel API stubs.
 * GENERATED by scripts/gen_stubs.py from linuxu/UNDEFINED_SYMBOLS.txt and
 * scripts/stub_policy.py. Do not edit; change the policy and regenerate:
 *   python3 scripts/gen_stubs.py
 * Each stub names its policy category. `unreachable` stubs log once and
 * return a benign value; no stub returns a negative errno through an
 * unsigned, enum, bool or pointer return type. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/printk.h>
"""

UNREACHABLE_HELPER = """
/* One-shot report for a stub the supported configuration should not reach. */
static void linuxu_stub_unreachable(bool *warned, const char *name)
{
	if (!__atomic_exchange_n(warned, true, __ATOMIC_RELAXED))
		pr_warn("linuxu: unexpected call to unsupported kernel API %s()\\n",
			name);
}
#define LINUXU_STUB_UNREACHABLE(name) do {				\\
	static bool __linuxu_warned;					\\
	linuxu_stub_unreachable(&__linuxu_warned, name);		\\
} while (0)
"""


def build(symbols, policy, includes, output):
    errors = []
    syms = set(symbols)
    unclassified = [s for s in symbols if s not in policy]
    if unclassified:
        errors.append("unclassified symbols (add them to scripts/stub_policy.py): "
                      + " ".join(unclassified))
    stale = sorted(s for s in policy if s not in syms)
    if stale:
        errors.append("policy entries not in UNDEFINED_SYMBOLS.txt: "
                      + " ".join(stale))
    for s in symbols:
        e = policy.get(s)
        if e is None:
            continue
        if e.get("cat") not in CATEGORIES:
            errors.append("%s: unknown category %r" % (s, e.get("cat")))
        elif e["cat"] == "implement":
            errors.append("%s: classified `implement`; it needs a real "
                          "implementation, not a generated stub" % s)
    if errors:
        raise PolicyError("\n".join(errors))

    classified = [s for s in symbols if s in policy]
    defs = find_definitions(classified, output)
    for s in classified:
        cat = policy[s]["cat"]
        if cat == "external" and not defs[s] and not policy[s].get("where"):
            errors.append("%s: classified external but no definition was "
                          "found outside the generated file" % s)
        if cat in GENERATED and defs[s]:
            errors.append("%s: classified %s but already defined in %s"
                          % (s, cat, ", ".join(defs[s])))
    if errors:
        raise PolicyError("\n".join(errors))

    out = []
    includes_seen = set()
    counts = {}
    for s in symbols:
        e = policy[s]
        cat = e["cat"]
        counts[cat] = counts.get(cat, 0) + 1
        if cat == "external":
            continue
        why = e.get("why", "")
        out.append("/* %s%s */" % (cat, ": " + why if why else ""))
        if cat == "data":
            out.append(e["def"])
            out.append("")
            continue
        if cat == "disabled-inline" and "body" in e:
            # The vendor !CONFIG stub is macro-generated (for example the
            # ACPICA ACPI_EXTERNAL_RETURN_* wrappers): the policy spells the
            # body and names a marker the vendor header must contain.
            vpath = os.path.join(VENDOR_INCLUDE, e["header"])
            if not os.path.exists(vpath) or e.get("marker", "\0") not in \
                    open(vpath).read():
                errors.append("%s: marker %r not found in third_party/linux/include/%s"
                              % (s, e.get("marker"), e["header"]))
                continue
            proto = find_prototype(s)
            if not proto:
                errors.append("%s: no declaration found" % s)
                continue
            rt, params, hdr = proto
            includes_seen.add(hdr)
            body = "\t" + e["body"]
        elif cat == "disabled-inline":
            rt, params, body = vendor_inline(s, e["header"])
        if cat == "disabled-inline":
            if return_kind(rt) != "void" and re.search(
                    r"return\s+-", body) and return_kind(rt) != "signed":
                errors.append("%s: vendor inline returns a negative value "
                              "through %s" % (s, rt))
            out.append("%s %s(%s)" % (rt, s, params))
            out.append("{")
            out.append(body if body.strip() else "")
            out.append("}")
            out.append("")
            continue
        if "proto" in e:
            m = re.match(r"^(.*?)\b" + re.escape(s) + r"\s*\((.*)\)\s*$",
                         " ".join(e["proto"].split()))
            if not m:
                errors.append("%s: malformed proto override" % s)
                continue
            rt, params = m.group(1).strip(), m.group(2).strip() or "void"
        else:
            proto = find_prototype(s)
            if not proto:
                errors.append("%s: no declaration found; add a `proto` "
                              "override" % s)
                continue
            rt, params, hdr = proto
            includes_seen.add(hdr)
        kind = return_kind(rt)
        if kind == "unknown":
            errors.append("%s: cannot classify return type %r" % (s, rt))
            continue
        ret = e.get("ret")
        if cat == "noop":
            if kind != "void":
                errors.append("%s: `noop` requires a void function, got %s"
                              % (s, rt))
                continue
            body = ["\t/* No effect in this environment. */"]
        elif cat == "ok-zero":
            if kind == "void":
                errors.append("%s: `ok-zero` on a void function; use noop" % s)
                continue
            body = ["\treturn %s;" % benign_value(kind, rt)]
        elif cat == "enosys":
            if kind != "signed":
                errors.append("%s: `enosys` needs a signed integer return, "
                              "got %s" % (s, rt))
                continue
            body = ["\treturn -ENOSYS;"]
        elif cat == "unreachable":
            body = ['\tLINUXU_STUB_UNREACHABLE("%s");' % s]
            if kind != "void":
                value = ret if ret is not None else benign_value(kind, rt)
                # The only negative value allowed through an unsigned type
                # is Linux's IS_ERR_VALUE() encoding for unsigned long
                # returns such as vm_mmap(); the policy must say so.
                err_encoded = e.get("err_encoded") and \
                    " ".join(rt.split()) == "unsigned long"
                if is_negative(value) and kind != "signed" and not err_encoded:
                    errors.append("%s: negative value %s through %s"
                                  % (s, value, rt))
                    continue
                body.append("\treturn %s;" % value)
        else:
            errors.append("%s: category %s cannot be generated" % (s, cat))
            continue
        if ret is not None and cat != "unreachable":
            errors.append("%s: `ret` is only valid for unreachable" % s)
            continue
        out.append("%s %s(%s)" % (rt, s, params))
        out.append("{")
        out.extend(body)
        out.append("}")
        out.append("")
    if errors:
        raise PolicyError("\n".join(errors))
    head = [HEADER]
    auto = set()
    for hdr in includes_seen:
        if hdr.startswith(SHADOW + os.sep):
            auto.add(os.path.relpath(hdr, SHADOW))
        else:
            auto.add(os.path.basename(hdr))
    for h in list(includes) + sorted(auto - set(includes)):
        head.append("#include <%s>" % h)
    head.append(UNREACHABLE_HELPER)
    return "\n".join(head + out).rstrip() + "\n", counts


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--symbols", default=SYMS_FILE)
    ap.add_argument("--policy", default=POLICY_FILE)
    ap.add_argument("--output", default=OUT_FILE)
    ap.add_argument("--check", action="store_true",
                    help="fail if the output file is not up to date")
    args = ap.parse_args()
    try:
        policy, includes = load_policy(args.policy)
        symbols = read_symbols(args.symbols)
        text, counts = build(symbols, policy, includes, args.output)
    except PolicyError as exc:
        print("gen_stubs: FAILED\n" + str(exc), file=sys.stderr)
        return 1
    summary = ", ".join("%s %d" % (c, counts[c]) for c in CATEGORIES
                        if c in counts)
    if args.check:
        current = open(args.output).read() if os.path.exists(args.output) else ""
        if current != text:
            print("gen_stubs: %s is out of date; run python3 "
                  "scripts/gen_stubs.py" % os.path.relpath(args.output, ROOT),
                  file=sys.stderr)
            return 1
        print("gen_stubs: policy OK, %s up to date (%s)"
              % (os.path.relpath(args.output, ROOT), summary))
        return 0
    with open(args.output, "w") as f:
        f.write(text)
    print("gen_stubs: wrote %s (%s)" % (os.path.relpath(args.output, ROOT),
                                        summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Verify the upstream Linux tree the build compiles.

Checks, against patches/manifest.json:
  * the submodule gitlink, the manifest pin and the submodule HEAD agree, and
    .gitmodules points at the declared repository;
  * the sparse checkout is exactly the declared pattern set, contains every
    source mk/upstream_sources.mk builds, and holds no two paths that differ
    only in case (they collide on the default case-insensitive APFS volume);
  * patches/linux/ holds exactly the declared patches, unchanged, and each is
    applied;
  * the submodule working tree differs from the pinned commit only by those
    patches: no other modified, deleted or untracked file;
  * the declared verbatim copies in linuxu/ still equal their upstream file;
  * the Makefile's compile interventions (lowercase -Dsymbol=replacement
    redirects and forced rt/ headers on upstream objects) match the declared
    list;
  * every declared CONFIG intervention (a symbol set against what upstream
    Kconfig would select for this target) names a Kconfig file that defines
    it and is set in linuxu/headers/linux/autoconf.h, so a declaration cannot
    go stale.
"""

import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "patches" / "manifest.json"
SOURCES_MK = ROOT / "mk" / "upstream_sources.mk"
MAKEFILE = ROOT / "Makefile"
AUTOCONF = ROOT / "linuxu" / "headers" / "linux" / "autoconf.h"
INTERVENTION = re.compile(r"-D[a-z_][A-Za-z0-9_]*=\S+|-include\s+rt/\S+")
OID = re.compile(r"[0-9a-f]{40}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")


class Failure(Exception):
    pass


def git(*args, cwd=ROOT, binary=False):
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True)
    if result.returncode:
        raise Failure(f"git {' '.join(args)} failed: {result.stderr.decode().strip()}")
    return result.stdout if binary else result.stdout.decode()


def report(label, items):
    for item in sorted(items):
        print(f"{label}: {item}", file=sys.stderr)


def check_pin(linux):
    path, pin = linux["path"], linux["pin"]
    if not OID.fullmatch(pin):
        raise Failure("manifest linux.pin must be a 40-digit commit")
    stage = git("ls-files", "-s", "--", path).split()
    if len(stage) < 2 or stage[0] != "160000":
        raise Failure(f"{path} is not a submodule gitlink")
    if stage[1] != pin:
        raise Failure(f"gitlink {stage[1]} differs from manifest pin {pin}")
    modules = git("config", "-f", ".gitmodules", "--get-regexp", r"^submodule\..*")
    if f"submodule.{path}.url {linux['url']}" not in modules.splitlines():
        raise Failure(f".gitmodules url for {path} differs from {linux['url']}")
    sub = ROOT / path
    if not (sub / ".git").exists():
        raise Failure(f"{path} is not checked out; run scripts/bootstrap.sh")
    head = git("rev-parse", "HEAD", cwd=sub).strip()
    if head != pin:
        raise Failure(f"{path} HEAD is {head}, expected {pin}; run scripts/bootstrap.sh")
    return sub


def check_sparse(sub, linux):
    configured = git("sparse-checkout", "list", cwd=sub).splitlines()
    if configured != linux["sparse_checkout"]:
        raise Failure("submodule sparse-checkout patterns differ from the manifest; "
                      "run scripts/bootstrap.sh")
    present = [line[2:] for line in git("ls-files", "-t", cwd=sub).splitlines()
               if line.startswith("H ")]
    folded = {}
    for path in present:
        folded.setdefault(path.lower(), []).append(path)
    collisions = [" / ".join(v) for v in folded.values() if len(v) > 1]
    if collisions:
        report("case collision in sparse checkout", collisions)
        raise Failure("the sparse checkout contains case-colliding paths")
    present = set(present)
    text = SOURCES_MK.read_text().replace("\\\n", " ")
    needed = set(re.findall(r"\$\(LINUX\)/(\S+)", text))
    for match in re.finditer(r"^UPSTREAM_HELPERS\s*:=(.*)$", text, re.M):
        needed |= {item.split(":", 1)[1] for item in match.group(1).split()}
    missing = needed - present
    if missing:
        report("build source outside the sparse checkout", missing)
        raise Failure("mk/upstream_sources.mk names files that are not checked out")
    return present, len(needed)


def patch_paths(text):
    return {m.group(1) for m in re.finditer(r"^diff --git a/(\S+) b/\S+$", text, re.M)}


def check_patches(sub, patches, present):
    declared = {}
    for item in patches:
        patch, files, reason = item.get("patch"), item.get("files"), item.get("reason")
        if (not isinstance(patch, str) or not patch.startswith("patches/linux/") or
                not patch.endswith(".patch") or ".." in Path(patch).parts or
                patch in declared or not isinstance(files, list) or not files or
                not isinstance(reason, str) or not reason.strip() or
                not SHA256.fullmatch(item.get("sha256", ""))):
            raise Failure(f"invalid patch declaration: {patch}")
        path = ROOT / patch
        if not path.is_file() or path.is_symlink():
            raise Failure(f"declared patch missing: {patch}")
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise Failure(f"declared patch changed (update its sha256): {patch}")
        touched = patch_paths(data.decode())
        if touched != set(files):
            raise Failure(f"{patch} touches {sorted(touched)}, declared {sorted(files)}")
        outside = touched - present
        if outside:
            raise Failure(f"{patch} touches files outside the sparse checkout: {sorted(outside)}")
        declared[patch] = item
    on_disk = {p.relative_to(ROOT).as_posix() for p in (ROOT / "patches" / "linux").glob("*")}
    if on_disk != set(declared):
        raise Failure(f"undeclared or missing patch files: {sorted(on_disk ^ set(declared))}")
    for patch in declared:
        result = subprocess.run(["git", "apply", "--check", "--reverse", str(ROOT / patch)],
                                cwd=sub, capture_output=True)
        if result.returncode:
            raise Failure(f"{patch} is not applied to the submodule; run scripts/bootstrap.sh")
    return declared


def check_working_tree(sub, declared):
    status = git("status", "--porcelain=v1", "-z", "--untracked-files=all",
                 "--ignore-submodules=all", cwd=sub)
    changed, untracked = set(), set()
    for entry in filter(None, status.split("\0")):
        code, path = entry[:2], entry[3:]
        (untracked if code == "??" else changed).add(path)
    expected = set()
    for item in declared.values():
        expected |= set(item["files"])
    if untracked:
        report("untracked file in submodule", untracked)
    stray = changed - expected
    if stray:
        report("modified outside the declared patches", stray)
    unapplied = expected - changed
    if unapplied:
        report("declared patch target unchanged", unapplied)
    if untracked or stray or unapplied:
        raise Failure("the submodule working tree differs from pin + declared patches")
    # Rebuild pin + patches in a scratch tree and require byte equality, so a
    # patched file carries no edits beyond its patch.
    scratch = Path(tempfile.mkdtemp(prefix="verify-upstream."))
    try:
        for path in expected:
            target = scratch / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(git("show", f"HEAD:{path}", cwd=sub, binary=True))
        for patch in declared:
            result = subprocess.run(["git", "apply", str(ROOT / patch)],
                                    cwd=scratch, capture_output=True)
            if result.returncode:
                raise Failure(f"{patch} does not apply to the pinned files: "
                              f"{result.stderr.decode().strip()}")
        differ = [p for p in expected
                  if (scratch / p).read_bytes() != (sub / p).read_bytes()]
        if differ:
            report("differs from pin + declared patches", differ)
            raise Failure("patched upstream files carry undeclared edits")
    finally:
        shutil.rmtree(scratch)
    return len(expected)


def check_copies(sub, copies):
    files = copies.get("files", {})
    if not isinstance(files, dict) or not copies.get("reason", "").strip():
        raise Failure("manifest verified_copies needs a reason and a files map")
    differ = [local for local, source in files.items()
              if not (ROOT / local).is_file() or not (sub / source).is_file()
              or (ROOT / local).read_bytes() != (sub / source).read_bytes()]
    if differ:
        report("verified copy differs from upstream", differ)
        raise Failure("a declared verbatim copy no longer matches the pinned file")
    return len(files)


def compile_interventions():
    """Return interventions the Makefile applies to upstream (non-linuxu) objects."""
    found = set()
    logical = MAKEFILE.read_text().replace("\\\n", " ")
    for line in logical.splitlines():
        if line.lstrip().startswith("#") or "/linuxu/" in line.split(":", 1)[0]:
            continue
        for match in INTERVENTION.finditer(line):
            found.add(" ".join(match.group(0).split()))
    return found


def check_interventions(declared):
    if not isinstance(declared, list):
        raise Failure("manifest compile_interventions must be a list")
    flags = set()
    for item in declared:
        flag, reason = item.get("flag"), item.get("reason")
        if (not isinstance(flag, str) or not INTERVENTION.fullmatch(flag) or
                flag in flags or not isinstance(reason, str) or not reason.strip()):
            raise Failure(f"invalid compile intervention declaration: {flag}")
        flags.add(flag)
    found = compile_interventions()
    report("undeclared compile intervention", found - flags)
    report("stale compile intervention", flags - found)
    if found != flags:
        raise Failure("Makefile compile interventions differ from the manifest")
    return len(flags)


def check_config_interventions(sub, declared):
    if not isinstance(declared, list):
        raise Failure("manifest config_interventions must be a list")
    autoconf = AUTOCONF.read_text()
    seen = set()
    for item in declared:
        config, kconfig, reason = item.get("config"), item.get("kconfig"), item.get("reason")
        if (not isinstance(config, str) or not re.fullmatch(r"CONFIG_[A-Z0-9_]+", config) or
                config in seen or not isinstance(kconfig, str) or
                not isinstance(reason, str) or not reason.strip()):
            raise Failure(f"invalid config intervention declaration: {config}")
        seen.add(config)
        kpath = sub / kconfig
        if not kpath.is_file() or not re.search(
                rf"^config {re.escape(config[len('CONFIG_'):])}$", kpath.read_text(), re.M):
            raise Failure(f"{config}: not defined by {kconfig}")
        if not re.search(rf"^#define {re.escape(config)} 1$", autoconf, re.M):
            raise Failure(f"{config}: declared but not set in {AUTOCONF.relative_to(ROOT)}")
    return len(seen)


def main():
    try:
        manifest = json.loads(MANIFEST.read_text())
        linux = manifest["linux"]
        sub = check_pin(linux)
        present, sources = check_sparse(sub, linux)
        declared = check_patches(sub, manifest["patches"], present)
        patched = check_working_tree(sub, declared)
        copies = check_copies(sub, manifest["verified_copies"])
        interventions = check_interventions(manifest["compile_interventions"])
        configs = check_config_interventions(sub, manifest.get("config_interventions", []))
    except (Failure, KeyError, OSError, ValueError) as error:
        print(f"upstream verification failed: {error}", file=sys.stderr)
        return 1
    print(f"Verified Linux {linux['pin']}: {len(present)} files checked out, "
          f"{sources} built sources present, {len(declared)} patch(es) on "
          f"{patched} file(s) and nothing else changed, {copies} verbatim copies, "
          f"{interventions} compile interventions, {configs} config interventions")
    return 0


if __name__ == "__main__":
    sys.exit(main())

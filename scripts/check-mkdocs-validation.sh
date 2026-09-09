#!/usr/bin/env bash
# `mkdocs build --strict` must be able to fail the way two files promise it does.
#
# `mkdocs.yml` and `.github/workflows/docs.yml` both state, in almost the same
# sentence, that `--strict` fails on a missing nav entry. It did not: mkdocs 1.6
# defaults `validation.nav.omitted_files` to `info`, so a page added under `docs/`
# and left out of `nav:` was reported at INFO and the build SUCCEEDED. Two files
# stating the same wrong thing is how it stayed wrong (#560).
#
# This asserts the PARSED configuration rather than grepping for a string, and it
# asserts the exemption too: `not_in_nav` must still cover the snippets, which are
# included into other pages rather than navigated to. Without that clause the
# validation fails the build on three files that are correct, which is how a
# validation flag gets turned back off by whoever meets it.
#
# It does NOT run mkdocs -- that is `.github/workflows/docs.yml`'s job and needs an
# install this suite cannot assume. What it guards is the configuration that makes
# that job able to fail at all.
set -euo pipefail

# `--root <tree>` points the check at a tree other than this checkout.
#
# It was spelled `--self-test` until #596, and it never ran a single case: there is no
# harness here, no staged fixture and no verdict table, only a root override. A flag named
# for a self-test that executes nothing is the same defect as an unregistered one, arriving
# from the other side -- the name asserts coverage and nothing behind it can fail. Nothing
# called it under either spelling, so the rename costs no call site.
root="$(cd "$(dirname "$0")/.." && pwd)"
[ "${1:-}" != "--root" ] || root="${2:?--root needs a tree}"

python3 - "$root" <<'PY'
import sys, os
try:
    import yaml
except ImportError:
    print("SKIP: PyYAML is not installed, so mkdocs.yml cannot be parsed")
    raise SystemExit(0)

root = sys.argv[1]
path = os.path.join(root, "mkdocs.yml")
if not os.path.isfile(path):
    print(f"FAIL {path} does not exist -- the scan is broken, not the tree")
    raise SystemExit(1)

# mkdocs.yml uses !!python/name: tags for the material theme; a plain safe_load
# refuses them, so unknown tags are ignored rather than resolved. Nothing this
# check reads is behind one.
class Loose(yaml.SafeLoader):
    pass
Loose.add_multi_constructor("!", lambda loader, suffix, node: None)
Loose.add_multi_constructor("tag:yaml.org,2002:python/name:", lambda l, s, n: None)

with open(path, encoding="utf-8") as handle:
    cfg = yaml.load(handle, Loader=Loose) or {}

failures = []

# `--strict` promotes a warning to a failure, so `warn` and `error` both fail CI
# and `info` does not. That is the whole defect.
level = (cfg.get("validation") or {}).get("nav", {}).get("omitted_files")
if level not in ("warn", "error"):
    failures.append(
        f"validation.nav.omitted_files is {level!r}; it must be 'warn' or 'error', or "
        "`--strict` cannot fail on a page missing from nav -- which mkdocs.yml and "
        "docs.yml both promise that it does")

# And the exemption, or the rule above fails a correct tree.
notInNav = cfg.get("not_in_nav") or ""
if "snippets" not in notInNav:
    failures.append(
        "not_in_nav does not cover snippets/, so the validation above would fail the "
        "build on files that are correctly absent from nav")

for line in failures:
    print("FAIL " + line)
raise SystemExit(1 if failures else 0)
PY
echo "check-mkdocs-validation: OK"

"""One-to-one HTML feature tests and their batch runner.

Each HTML page in ``ci/feature_html`` sets ``window.__FEATURE_TEST__`` to::

    {name, done, pass, cases: [{name, pass, detail}]}

The runner opens every page in isolation and prints one line per case.
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
from pathlib import Path

from ruyijs import Context

ROOT = Path(__file__).resolve().parent
HTML_DIR = ROOT / "feature_html"
SHELL = Path(os.environ.get("RUYIJS_SHELL", ROOT.parent / "dist" / "js.exe"))
TEST_URL = "https://feature.test/"

RESULT_SCRIPT = r"""
(() => {
  const R = window.__FEATURE_TEST__ || {name: 'unknown', done: false, cases: []};
  return {
    name: R.name,
    done: R.done,
    pass: R.done ? R.pass === true && R.cases.every(c => c.pass === true) : false,
    cases: (R.cases || []).map(c => ({
      name: c.name,
      pass: c.pass === true,
      detail: c.detail === undefined ? '' : String(c.detail),
    })),
  };
})()
"""


def options_for(path: Path, workdir: Path) -> dict:
    prefix = path.stem.split("_", 1)[0] + "_"
    marker = "<!-- options: "
    text = path.read_text(encoding="utf-8")
    options = {}
    start = text.find(marker)
    if start >= 0:
        end = text.find(" -->", start)
        if end >= 0:
            options = json.loads(text[start + len(marker):end])
    environment = options.get("environment")
    if environment is None:
        environment = options.get("fingerprint", {})
    else:
        environment = dict(environment)
        for key, value in list(environment.items()):
            if isinstance(value, str):
                environment[key] = value.replace("{DIR}", workdir.as_posix())
    return {
        "environment": environment,
        "mode": options.get("mode", "default"),
        "expect_files": options.get("expect_files", []),
        "expect_api": options.get("expect_api", []),
        "resources": options.get("resources", []),
        "require_feature_shell": options.get("require_feature_shell", False),
        "env": {key: str(value) for key, value in (options.get("environment") or {}).items()
                if key.startswith("RUYIJS_")},
        "prefix": prefix,
    }


def run_one(path: Path, shell: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="ruyijs-html-") as temp:
        workdir = Path(temp)
        options = options_for(path, workdir)
        html = path.read_text(encoding="utf-8")
        if "<head" not in html.lower():
            html = html.replace("<html", "<html><head></head>", 1)
            if "<head" not in html.lower():
                html = "<head></head>" + html
        if "<body" not in html.lower():
            insert = html.lower().find("<script")
            if insert < 0:
                insert = len(html)
            html = html[:insert] + "<body>" + html[insert:] + "</body>"
        html = html.replace(
            "</script>",
            "</scr" + "ipt><script>window.__FEATURE_TEST_SOURCE__ = true;</script>",
            1,
        )
        process_env = {}
        context_env = {}
        for key, value in (options["environment"] or {}).items():
            if key.startswith("RUYIJS_"):
                process_env[key] = str(value)
            else:
                context_env[key] = value
        keys = list(process_env)
        saved = {key: os.environ.get(key) for key in keys}
        os.environ.update(process_env)
        try:
            with Context(shell=str(shell), environment=context_env or None,
                         mode=options["mode"]) as ctx:
                for resource in options["resources"]:
                    url, body, headers = resource
                    ctx.put_resource(url, body, headers=headers)
                ctx.load_page(TEST_URL, html)
                for _ in range(100):
                    result = ctx.eval(RESULT_SCRIPT)
                    if result["done"]:
                        break
                    ctx.advance(20)
                else:
                    result = ctx.eval(RESULT_SCRIPT)
        finally:
            for key, value in saved.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
        result["name"] = options["prefix"] + result.get("name", path.stem)
        missing = [name for name in options["expect_files"] if not (workdir / name).is_file()]
        if missing:
            result["pass"] = False
            result.setdefault("cases", []).append({
                "name": "trace file created",
                "pass": False,
                "detail": "missing: " + ", ".join(missing),
            })
        result["blocked"] = [name for name in options["expect_api"]
                              if name not in getattr(run_one, "supported_api", set())]
        return result


def main() -> int:
    if not SHELL.is_file():
        print("BLOCKED: shell not found: " + str(SHELL))
        print("Build the feature binary or set RUYIJS_SHELL before running this runner.")
        return 2
    files = sorted(HTML_DIR.glob("*.html"))
    if not files:
        print("BLOCKED: no HTML tests in " + str(HTML_DIR))
        return 2
    feature_shell = (ROOT.parent / "dist" / SHELL.name).resolve()
    using_feature_shell = SHELL.resolve() == feature_shell
    probe_dir = Path(tempfile.mkdtemp(prefix="ruyijs-html-probe-"))
    supported = set()
    for name in sorted({item for path in files for item in options_for(path, probe_dir)["expect_api"]}):
        target = probe_dir / (name + ".jsonl")
        saved = os.environ.get(name)
        os.environ[name] = str(target)
        try:
            with Context(shell=str(SHELL)):
                pass
            if target.is_file():
                supported.add(name)
        finally:
            if saved is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = saved
    run_one.supported_api = supported
    passed = []
    failed = []
    blocked = []
    for path in files:
        options = options_for(path, probe_dir)
        if options["require_feature_shell"] and not using_feature_shell:
            blocked.append((path.stem, "requires " + str(feature_shell) +
                            "; feature build is blocked by missing engine APIs"))
            continue
        try:
            result = run_one(path, SHELL)
        except Exception as error:  # broken runtime or unsupported API
            blocked.append((path.stem, str(error)))
            continue
        if result.get("blocked"):
            blocked.append((path.stem, "missing engine support: " + ", ".join(result["blocked"])))
        elif result["pass"]:
            passed.append(path.stem)
        else:
            failed.append((path.stem, result["cases"]))
    print("HTML FEATURE TEST SUMMARY")
    print("PASS " + str(len(passed)))
    print("FAIL " + str(len(failed)))
    print("BLOCKED " + str(len(blocked)))
    for name in passed:
        print("PASS " + name)
    for name, cases in failed:
        print("FAIL " + name)
        for case in cases:
            if not case["pass"]:
                print("    - " + case["name"] + ": " + case["detail"])
    for name, reason in blocked:
        print("BLOCKED " + name + ": " + reason)
    output = os.environ.get("RUYIJS_HTML_RESULT_JSON")
    if output:
        Path(output).write_text(json.dumps({
            "passed": passed,
            "failed": [{"name": name, "cases": cases} for name, cases in failed],
            "blocked": [{"name": name, "reason": reason} for name, reason in blocked],
        }, indent=2), encoding="utf-8")
    return 0 if not failed and not blocked else 1


if __name__ == "__main__":
    sys.exit(main())

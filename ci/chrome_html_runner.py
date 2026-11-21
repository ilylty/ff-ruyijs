"""Run the shared feature HTML tests in Chrome through CDP.

The pages are served over a local HTTP server so relative URLs and resource
options behave like a browser page. Browser-only differences are reported as
failures; this runner does not emulate ruyijs fingerprint or trace options.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

import websocket

ROOT = Path(__file__).resolve().parent
HTML_DIR = ROOT / "feature_html"
CHROME = Path(os.environ.get("CHROME_EXE", r"F:/chromium/src/out/default/chrome.exe"))
OUTPUT = Path(os.environ.get("CHROME_HTML_RESULT_JSON", ROOT.parent / "artifacts" / "chrome-html-results.json"))
RESULT_SCRIPT = r"""
(() => {
  const R = window.__FEATURE_TEST__ || {name: 'unknown', done: false, cases: []};
  return {
    name: R.name,
    done: R.done === true,
    pass: R.done === true && R.pass === true && (R.cases || []).every(c => c.pass === true),
    cases: (R.cases || []).map(c => ({
      name: c.name,
      pass: c.pass === true,
      detail: c.detail === undefined ? '' : String(c.detail),
    })),
  };
})()
"""


def options_for(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    marker = "<!-- options: "
    start = text.find(marker)
    if start < 0:
        return {"resources": [], "expect_files": [], "fingerprint": {}}
    end = text.find(" -->", start)
    if end < 0:
        return {"resources": [], "expect_files": [], "fingerprint": {}}
    options = json.loads(text[start + len(marker):end])
    options.setdefault("resources", [])
    options.setdefault("expect_files", [])
    options.setdefault("fingerprint", {})
    return options


class TestServer:
    def __init__(self, files: list[Path]):
        self.files = {path.stem: path for path in files}
        self.resources = {}
        for path in files:
            for item in options_for(path)["resources"]:
                url, body, headers = item
                self.resources[urlparse(url).path] = (body, dict(headers))
        handler = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_GET(self):
                path = urlparse(self.path).path
                if path.endswith(".html"):
                    stem = Path(path).stem
                    source = handler.files.get(stem)
                    if source is None:
                        self.send_error(404)
                        return
                    body = source.read_bytes()
                    headers = {"Content-Type": "text/html; charset=utf-8"}
                elif path in handler.resources:
                    body, headers = handler.resources[path]
                    body = body.encode("utf-8") if isinstance(body, str) else body
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                for key, value in headers.items():
                    self.send_header(key, value)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_args):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=2)


class CDP:
    def __init__(self, ws_url: str):
        self.ws = websocket.create_connection(ws_url, timeout=15, suppress_origin=True)
        self.ident = 0

    def call(self, method: str, params: dict | None = None, session: str | None = None):
        self.ident += 1
        ident = self.ident
        message = {"id": ident, "method": method, "params": params or {}}
        if session:
            message["sessionId"] = session
        self.ws.send(json.dumps(message))
        while True:
            message = json.loads(self.ws.recv())
            if message.get("id") == ident:
                if "error" in message:
                    raise RuntimeError(message["error"])
                return message.get("result", {})

    def close(self):
        self.ws.close()


def launch_chrome() -> tuple[subprocess.Popen, CDP, str]:
    profile = Path(tempfile.mkdtemp(prefix="chrome-html-profile-"))
    command = [
        str(CHROME),
        "--headless=new",
        f"--user-data-dir={profile}",
        "--remote-debugging-port=0",
        "--remote-allow-origins=*",
        "--no-first-run",
        "--no-default-browser-check",
        "--enable-unsafe-swiftshader",
        "about:blank",
    ]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    port_file = profile / "DevToolsActivePort"
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if port_file.is_file():
            break
        time.sleep(0.1)
    else:
        process.kill()
        raise RuntimeError("Chrome DevToolsActivePort was not created")
    port = int(port_file.read_text(encoding="utf-8").splitlines()[0])
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/version", timeout=10) as response:
        version = json.load(response)
    cdp = CDP(version["webSocketDebuggerUrl"])
    target = cdp.call("Target.createTarget", {"url": "about:blank"})["targetId"]
    session = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
    cdp.call("Runtime.enable", session=session)
    cdp.call("Page.enable", session=session)
    return process, cdp, session


def evaluate(cdp: CDP, session: str, expression: str):
    result = cdp.call("Runtime.evaluate", {
        "expression": expression,
        "returnByValue": True,
        "awaitPromise": False,
    }, session=session)
    if "exceptionDetails" in result:
        detail = result["exceptionDetails"].get("text", "exception")
        raise RuntimeError(detail)
    return result.get("result", {}).get("value")


def run_page(cdp: CDP, session: str, base: str, path: Path) -> dict:
    url = f"{base}/{path.name}"
    cdp.call("Page.navigate", {"url": url}, session=session)
    deadline = time.monotonic() + 8
    result = {"name": path.stem, "done": False, "pass": False, "cases": []}
    while time.monotonic() < deadline:
        time.sleep(0.03)
        try:
            result = evaluate(cdp, session, RESULT_SCRIPT) or result
        except RuntimeError:
            continue
        if result.get("done"):
            break
    options = options_for(path)
    result["host_file_checks"] = options["expect_files"]
    result["name"] = path.stem
    result["pass"] = bool(result.get("done") and result.get("pass") and
                          all(case.get("pass") for case in result.get("cases", [])))
    return result


def main() -> int:
    if not CHROME.is_file():
        print("Chrome not found: " + str(CHROME))
        return 2
    files = sorted(HTML_DIR.glob("*.html"))
    if not files:
        print("No HTML tests in " + str(HTML_DIR))
        return 2
    results = {}
    with TestServer(files) as server, tempfile.TemporaryDirectory(prefix="chrome-html-") as _profile:
        process, cdp, session = launch_chrome()
        try:
            base = f"http://127.0.0.1:{server.port}"
            for path in files:
                try:
                    results[path.stem] = run_page(cdp, session, base, path)
                except Exception as error:
                    results[path.stem] = {
                        "name": path.stem,
                        "done": False,
                        "pass": False,
                        "cases": [{"name": "runner", "pass": False, "detail": str(error)}],
                    }
        finally:
            cdp.close()
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()

    passed = [name for name, result in results.items() if result["pass"]]
    failed = [name for name, result in results.items() if not result["pass"]]
    print("CHROME HTML TEST SUMMARY")
    print(f"PASS {len(passed)}")
    print(f"FAIL {len(failed)}")
    for name in passed:
        print("PASS " + name)
    for name in failed:
        print("FAIL " + name)
        for case in results[name]["cases"]:
            if not case["pass"]:
                print("    - " + case["name"] + ": " + case["detail"])
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())

"""Test the split engine patches: WindowProxy glue and standalone trace.

Three independent checks, all against the built feature shell:

1. WindowProxy: `window` is the realm's real Proxy, globalThis and
   document.defaultView resolve to it, and it is reported as a Proxy by
   structuredClone's failure mode.
2. Call trace: RUYIJS_DOM_TRACE emits jscall records for JS/native calls.
3. Dynamic-code trace: RUYIJS_DOM_EVAL_TRACE records eval/Function source.
4. WASM trace: RUYIJS_DOM_WASM_TRACE emits wasm records for instantiate.

Exit status is non-zero on the first failed check.
"""
import json
import os
import sys
import tempfile
from pathlib import Path

from ruyijs import Context

SHELL = os.environ.get("RUYIJS_SHELL", "J:/workspace/ff-ruyijs-feature/dist/js.exe")
PAGE = "<!doctype html><html><head></head><body><div id=a>x</div></body></html>"
URL = "https://engine-patch.test/"


def check(label, condition, detail=""):
    ok = bool(condition)
    print(("ok   " if ok else "FAIL ") + label + (f"  ({detail})" if detail else ""), flush=True)
    if not ok:
        raise SystemExit(1)


def with_env(name, value, fn):
    saved = os.environ.get(name)
    os.environ[name] = str(value)
    try:
        return fn()
    finally:
        if saved is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = saved


def check_proxy():
    with Context(shell=SHELL) as ctx:
        ctx.load_page(URL, PAGE)
        state = ctx.eval("""JSON.stringify((() => {
          let proxyClone = null;
          try { structuredClone(window); } catch (e) { proxyClone = e.name + ':' + e.message; }
          return {
            winIsGlobal: window === globalThis,
            defaultView: document.defaultView === window,
            tag: Object.prototype.toString.call(window),
            proxyClone,
          };
        })())""")
    state = json.loads(state)
    check("window === globalThis", state["winIsGlobal"], state)
    check("document.defaultView === window", state["defaultView"], state)
    check("window has Window tag", state["tag"] == "[object Window]", state["tag"])
    check("window is a Proxy", "Proxy object could not be cloned" in (state["proxyClone"] or ""),
          state["proxyClone"])


def check_call_trace():
    with tempfile.TemporaryDirectory(prefix="engine-trace-") as tmp:
        path = Path(tmp) / "calls.jsonl"

        def run():
            with Context(shell=SHELL) as ctx:
                ctx.load_page(URL, PAGE)
                ctx.eval("function add(a,b){return a+b} globalThis.__r = add(1,2);")
            return path

        with_env("RUYIJS_DOM_TRACE", path, run)
        check("call trace file exists", path.is_file())
        lines = path.read_text(encoding="utf-8").splitlines() if path.is_file() else []
        records = [json.loads(line) for line in lines if line.strip()]
        types = {record.get("type") for record in records}
        check("call trace has records", len(records) > 0, len(records))
        check("call trace has jscall frames", "jscall_frame" in types, sorted(types))
        has_add = any(record.get("callee_name") == "add" for record in records)
        check("call trace records script callee", has_add)


def check_eval_trace():
    with tempfile.TemporaryDirectory(prefix="engine-eval-trace-") as tmp:
        path = Path(tmp) / "eval.jsonl"

        def run():
            with Context(shell=SHELL) as ctx:
                ctx.load_page(URL, PAGE)
                ctx.eval("eval('globalThis.__eval_marker = 0x5eed;');")
            return path

        with_env("RUYIJS_DOM_EVAL_TRACE", path, run)
        check("eval trace file exists", path.is_file())
        text = path.read_text(encoding="utf-8") if path.is_file() else ""
        check("eval trace has records", len(text.strip().splitlines()) > 0, len(text))
        # The source is recorded as dynamicCode; either the literal marker or
        # its JSON string form is acceptable.
        check("eval trace records source", "__eval_marker" in text, text[:200])


def check_wasm_trace():
    with tempfile.TemporaryDirectory(prefix="engine-wasm-trace-") as tmp:
        path = Path(tmp) / "wasm.jsonl"

        def run():
            with Context(shell=SHELL) as ctx:
                ctx.load_page(URL, PAGE)
                ctx.eval("""
                  (() => {
                    const bytes = new Uint8Array([0,97,115,109,1,0,0,0]);
                    return WebAssembly.instantiate(bytes, {}).then(() => true);
                  })()
                """)
                for _ in range(100):
                    if ctx.eval("globalThis.__done === true"):
                        break
                    ctx.advance(10)
            return path

        with_env("RUYIJS_DOM_WASM_TRACE", path,
                 lambda: with_env("RUYIJS_DOM_WASM_DUMP_DIR", tmp, run))
        check("wasm trace file exists", path.is_file())
        lines = path.read_text(encoding="utf-8").splitlines() if path.is_file() else []
        types = {json.loads(line).get("type") for line in lines if line.strip()}
        check("wasm trace has wasm records", "wasm" in types, sorted(types))


def check_native_instantiate_streaming():
    """WebAssembly.instantiateStreaming stays native; host consumes Response."""
    with Context(shell=SHELL) as ctx:
        ctx.load_page(URL, PAGE)
        text = ctx.eval("String(WebAssembly.instantiateStreaming)")
        check("instantiateStreaming is native", "[native code]" in text, text[:80])
        ctx.eval("""
          globalThis.__wasm = 'pending';
          (() => {
            const bytes = new Uint8Array([0,97,115,109,1,0,0,0]);
            const headers = {'Content-Type': 'application/wasm'};
            WebAssembly.instantiateStreaming(new Response(bytes, {headers}), {})
              .then(result => {
                if (!(result.module instanceof WebAssembly.Module)) {
                  globalThis.__wasm = 'bad-module'; return;
                }
                const wrong = new Response(bytes,
                    {headers: {'Content-Type': 'text/plain'}});
                return WebAssembly.instantiateStreaming(wrong, {}).then(
                  () => { globalThis.__wasm = 'wrong-mime-resolved'; },
                  error => { globalThis.__wasm =
                      /MIME type/.test(String(error && error.message))
                        ? 'ok' : 'wrong-mime-error:' + error.message; });
              }, error => { globalThis.__wasm = 'error:' + error.message; });
          })()
        """)
        for _ in range(100):
            value = ctx.eval("globalThis.__wasm")
            if value != "pending":
                break
            ctx.advance(10)
    check("native instantiateStreaming(Response) works", value == "ok", value)


def main():
    check_proxy()
    check_call_trace()
    check_eval_trace()
    check_wasm_trace()
    check_native_instantiate_streaming()
    print("engine patch checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

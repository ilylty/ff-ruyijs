"""Generate one-to-one HTML feature test pages."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "feature_html"
HARNESS = '''(() => {
  const R = {name: __NAME__, done: false, cases: []};
  window.__FEATURE_TEST__ = R;
  const add = (name, fn) => {
    try {
      const detail = fn();
      R.cases.push({name, pass: detail === true, detail: detail === true ? "" : detail});
    } catch (e) {
      R.cases.push({name, pass: false, detail: String(e && e.message || e)});
    }
  };
  __BODY__
  R.done = true;
  R.pass = R.cases.every(c => c.pass);
})();
'''


def page(name: str, body: str, options: str | None = None, prefix: str = "") -> None:
    source = HARNESS.replace("__NAME__", repr(name)).replace("__BODY__", body.strip())
    parts = ["<!doctype html><html><head>"]
    if prefix:
        parts.append(prefix.strip())
    if options:
        parts.insert(1, "<!-- options: " + options + " -->")
    parts.append("<meta charset=\"utf-8\">")
    parts.append("</head><body>")
    parts.append("<script>")
    parts.append(source)
    parts.append("</script>")
    parts.append("</body></html>")
    (ROOT / (name + ".html")).write_text("\n".join(parts) + "\n", encoding="utf-8")


page("001_window_proxy", r'''
add("window is globalThis", () => window === globalThis);
add("window toString tag", () => Object.prototype.toString.call(window) === "[object Window]");
add("document.defaultView is window", () => document.defaultView === window);
add("window.document is document", () => window.document === document);
add("window has a non-Object prototype", () => Object.getPrototypeOf(window) !== Object.prototype);
''')

page("002_structured_clone_proxy", r'''
add("structuredClone(window) uses Gecko proxy message", () => {
  let error;
  try { structuredClone(window); } catch (e) { error = e; }
  return !!error && error.name === "DataCloneError" &&
      /Proxy object could not be cloned/.test(error.message);
});
''')

page("003_noscript_render", r'''
document.body.innerHTML = [
  "<p id=n><noscript>HIDDEN</noscript></p>",
  "<p id=s><scr" + "ipt>/*x*/</scr" + "ipt><style>A{}</style></p>"
].join("");
const n = document.getElementById("n");
add("noscript textContent keeps raw text", () => n.textContent === "HIDDEN");
add("noscript innerText is empty", () => n.innerText === "");
add("noscript has no offset box", () => document.querySelector("noscript").offsetWidth === 0 &&
                                      document.querySelector("noscript").offsetHeight === 0);
add("raw text children have no offset box", () => document.querySelector("script").offsetHeight === 0 &&
                                                document.querySelector("style").offsetHeight === 0);
''')

page("004_window_geometry", r'''
add("outerWidth from window group", () => outerWidth === 1200);
add("outerHeight from window group", () => outerHeight === 800);
add("screenX from window group", () => screenX === 30);
add("screenY from window group", () => screenY === 40);
add("innerWidth from viewport group", () => innerWidth === 1000);
add("mozInnerScreenX uses frame origin", () => mozInnerScreenX === 38);
add("mozInnerScreenY uses frame origin", () => mozInnerScreenY === 232);
''', r'''{"fingerprint":{"window":{"outerWidth":1200,"outerHeight":800,"screenX":30,"screenY":40},"viewport":{"width":1000,"height":600,"devicePixelRatio":1}}}''')

page("005_locale_language_wins", r'''
add("navigator.language is configured", () => navigator.language === "de-DE");
add("Intl follows differing navigator primary language", () =>
    Intl.DateTimeFormat().resolvedOptions().locale === "de-DE");
''', r'''{"fingerprint":{"navigator":{"language":"de-DE"},"locale":{"timezone":"Asia/Shanghai","osLocale":"en-US"}}}''')

page("006_locale_os_wins", r'''
add("same primary language keeps OS region", () => navigator.language === "en-GB");
add("Intl uses OS locale region", () => Intl.DateTimeFormat().resolvedOptions().locale === "en-US");
''', r'''{"fingerprint":{"navigator":{"language":"en-GB"},"locale":{"timezone":"Asia/Shanghai","osLocale":"en-US"}}}''')

page("007_computed_style_main_source", r'''
document.body.innerHTML = "<div id=a>text</div>";
const style = getComputedStyle(document.getElementById("a"));
add("computed view follows Firefox shape", () => style.length > 85);
add("computed view exposes indexed names", () => Object.keys(style).length === style.length && style[0] === style.item(0));
add("named read still works", () => typeof style.color === "string");
''')

page("008_rule_style_indexed", r'''
document.head.innerHTML = "<style id=s>#a { color: rgb(1, 2, 3); margin-left: 7px }</style>";
document.body.innerHTML = "<div id=a></div>";
const style = document.styleSheets[0].cssRules[0].style;
const before = Object.keys(style).slice();
style.setProperty("color", "rgb(4, 5, 6)");
const after = Object.keys(style).slice();
add("rule view exposes indexed declaration names", () => before.length === 2 && before.includes("0"));
add("setProperty refreshes indexed names", () => after.length === 2 && style[0] === "color");
add("cssText reflects setProperty", () => style.cssText === "color: rgb(4, 5, 6); margin-left: 7px;");
''')

page("009_trusted_types_csp", r'''
add("trustedTypes is installed", () => typeof trustedTypes === "object" && trustedTypes !== null);
add("CSP allows named policy", () => {
  try { trustedTypes.createPolicy("allowed", {createHTML: s => s}); return true; }
  catch (e) { return String(e.message); }
});
add("CSP blocks unnamed policy", () => {
  try { trustedTypes.createPolicy("blocked", {createHTML: s => s}); return "policy was created"; }
  catch (e) { return /Content-Security-Policy blocked/.test(e.message); }
});
''', prefix=r'''<meta http-equiv="Content-Security-Policy" content="trusted-types allowed">''')

page("010_media_keys", r'''
add("requestMediaKeySystemAccess exists", () => typeof navigator.requestMediaKeySystemAccess === "function");
add("method arity is 2", () => navigator.requestMediaKeySystemAccess.length === 2);
add("MediaKeySystemAccess constructor exists", () => typeof MediaKeySystemAccess === "function");
add("MediaKeys constructor exists", () => typeof MediaKeys === "function");
''')

page("011_webgl_internalformat", r'''
const gl = document.createElement("canvas").getContext("webgl2");
add("WebGL2 is available", () => !!gl);
if (gl) {
  add("RGBA8 SAMPLES is descending powers of two", () => {
    const a = Array.from(gl.getInternalformatParameter(gl.RENDERBUFFER, gl.RGBA8, gl.SAMPLES));
    return a.length > 0 && a.every((v, i) => i === 0 || a[i - 1] > v);
  });
  add("STENCIL_INDEX8 sets INVALID_ENUM", () => {
    gl.getError();
    gl.getInternalformatParameter(gl.RENDERBUFFER, 0x84F9, gl.SAMPLES);
    return gl.getError() === gl.INVALID_ENUM;
  });
}
''')

page("012_element_insert_receiver", r'''
document.body.innerHTML = "<div id=a></div>";
const div = document.getElementById("a");
add("insertAdjacentHTML works", () => {
  div.insertAdjacentHTML("beforeend", "<b>ok</b>");
  return div.innerHTML.includes("<b>ok</b>");
});
add("wrong receiver is rejected", () => {
  try { Element.prototype.insertAdjacentHTML.call({}, "beforeend", "<b>x</b>"); return "accepted"; }
  catch (e) { return e.name === "TypeError"; }
});
''')

page("013_getcomputedstyle_receiver", r'''
document.body.innerHTML = "<div id=a></div>";
add("computed style returns a declaration view", () =>
    typeof getComputedStyle(document.getElementById("a")).display === "string");
add("wrong receiver is rejected", () => {
  try { getComputedStyle.call({}, document.getElementById("a")); return "accepted"; }
  catch (e) { return e.name === "TypeError"; }
});
''')

page("014_selection_live_range", r'''
document.body.innerHTML = "<div id=a>text</div>";
const range = document.createRange();
range.selectNode(document.getElementById("a"));
const selection = getSelection();
selection.addRange(range);
add("selection has one range", () => selection.rangeCount === 1);
add("getRangeAt returns the same Range", () => selection.getRangeAt(0) === range);
add("selection type is Range", () => selection.type === "Range");
add("anchor node is live", () => !!selection.anchorNode);
''', r'''{"require_feature_shell":true}''')

page("015_shadow_root_point_query", r'''
document.body.innerHTML = "<div id=a></div>";
const root = document.getElementById("a").attachShadow({mode: "open"});
root.innerHTML = "<span id=b>shadow</span>";
add("shadow root exposes elementFromPoint", () => typeof root.elementFromPoint === "function");
add("shadow root exposes elementsFromPoint", () => typeof root.elementsFromPoint === "function");
add("shadow content is queryable", () => root.querySelector("#b").textContent === "shadow");
''')

page("016_timer_basic", r'''
let ran = 0;
const id = setTimeout(() => { ran = 1; }, 10);
clearTimeout(id);
setTimeout(() => {
  add("setTimeout returns a numeric handle", () => typeof id === "number");
  add("cleared timer did not run", () => ran === 0);
  R.done = true;
  R.pass = R.cases.every(c => c.pass);
}, 30);
''')

page("017_postmessage_basic", r'''
let seen = null;
window.addEventListener("message", event => { seen = event.data; }, true);
window.postMessage("hello", "*");
add("window is a PageProxy wrapper", () => Object.prototype.toString.call(window) === "[object Window]");
setTimeout(() => {
  add("window message is delivered", () => seen === "hello");
  R.done = true;
  R.pass = R.cases.every(c => c.pass);
}, 20);
''')

page("018_search_params", r'''
const p = new URLSearchParams("b=2&a=1");
p.append("a", "3");
p.sort();
add("URLSearchParams sorts in place", () => p.toString() === "a=1&a=3&b=2");
add("URLSearchParams round-trips", () => new URLSearchParams(p.toString()).getAll("a").join(",") === "1,3");
''')

page("019_url_conversion", r'''
const u = new URL("../next?x=1", "https://example.test/a/b");
add("URL resolves relative href", () => u.href === "https://example.test/next?x=1");
add("URLSearchParams parses query", () => u.searchParams.get("x") === "1");
''')

page("020_cf_access_trace", r'''
document.title = "cf-access";
const d = document.createElement("div");
d.id = "target";
document.body.appendChild(d);
void d.id;
add("page ran", () => document.title === "cf-access");
''', r'''{"environment":{"RUYIJS_CF_TRACE":"{DIR}/cf_access.jsonl"},"expect_files":["cf_access.jsonl"]}''')

page("021_dom_event_trace", r'''
let count = 0;
const target = document.createElement("button");
target.addEventListener("click", () => { count += 1; });
target.click();
add("listener ran", () => count === 1);
''', r'''{"environment":{"RUYIJS_DOM_EVENT_TRACE":"{DIR}/event.jsonl"},"expect_files":["event.jsonl"]}''')

page("022_cookie_trace", r'''
document.cookie = "trace=1";
add("cookie write is visible", () => document.cookie.includes("trace=1"));
''', r'''{"environment":{"RUYIJS_DOM_COOKIE_TRACE":"{DIR}/cookie.jsonl"},"expect_files":["cookie.jsonl"]}''')

page("023_storage_trace", r'''
let request = null;
try { request = indexedDB.open("feature_html_storage"); } catch (e) {}
setTimeout(() => {
  add("indexedDB.open returned a request", () => !!request);
  R.done = true;
  R.pass = R.cases.every(c => c.pass);
}, 20);
''', r'''{"environment":{"RUYIJS_DOM_STORAGE_TRACE":"{DIR}/storage.jsonl"},"expect_files":["storage.jsonl"]}''')

page("024_dom_transport_trace", r'''
fetch("/trace-fetch").then(r => r.text()).then(text => {
  add("fetch response is readable", () => text === "trace payload");
  R.done = true;
  R.pass = R.cases.every(c => c.pass);
});
''', r'''{"environment":{"RUYIJS_DOM_TRANSPORT_TRACE":"{DIR}/transport.jsonl"},"expect_files":["transport.jsonl"],"resources":[["https://feature.test/trace-fetch","trace payload",[["Content-Type","text/plain"]]]]}''')

page("025_timer_trace", r'''
setTimeout(() => {
  add("timer ran", () => true);
  R.done = true;
  R.pass = R.cases.every(c => c.pass);
}, 10);
''', r'''{"environment":{"RUYIJS_CF_TRACE":"{DIR}/timer.jsonl"},"expect_files":["timer.jsonl"]}''')

page("026_frame_enumeration", r'''
document.body.innerHTML = "<iframe></iframe>";
add("undefined is own key from runtime head", () => Object.prototype.hasOwnProperty.call(window, "undefined"));
add("boolean globals are own keys", () => ["Boolean", "JSON", "Date", "Math", "RegExp"]
  .every(name => Object.prototype.hasOwnProperty.call(window, name)));
add("frames is an own key", () => Object.prototype.hasOwnProperty.call(window, "frames"));
add("window exposes numeric frame indexes", () => "0" in window && window.frames.length === 1);
add("numeric frame index resolves to a WindowProxy", () => window[0] === window.frames[0]);
''')

page("028_wasm_streaming_response_patch", r'''
const moduleBytes = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00
]);
function wasmResponse(bytes) {
  return new Response(bytes, {headers: {"Content-Type": "application/wasm"}});
}
add("instantiateStreaming(Response) returns a Promise", () => {
  const pending = WebAssembly.instantiateStreaming(wasmResponse(moduleBytes), {});
  return pending && typeof pending.then === "function";
});
function finish() {
  R.done = true;
  R.pass = R.cases.every(c => c.pass);
}
WebAssembly.instantiateStreaming(wasmResponse(moduleBytes), {}).then(result => {
  add("instantiate returns a WebAssembly.Module", () => result.module instanceof WebAssembly.Module);
  add("instantiate returns a WebAssembly.Instance", () => result.instance instanceof WebAssembly.Instance);
  return WebAssembly.instantiateStreaming(
      Promise.resolve(wasmResponse(moduleBytes)), {});
}).then(result => {
  add("Promise<Response> is accepted", () =>
      result.module instanceof WebAssembly.Module &&
      result.instance instanceof WebAssembly.Instance);
  const bad = new Response(moduleBytes, {headers: {"Content-Type": "text/plain"}});
  return WebAssembly.instantiateStreaming(bad, {}).then(
      () => add("wrong MIME is rejected", () => "resolved"),
      error => add("wrong MIME is rejected", () =>
          /MIME type/.test(String(error && error.message || error))));
}).then(finish, error => {
  add("Response path rejects", () => String(error && error.message || error));
  finish();
});
''')

page("029_window_receiver_apis", r'''
add("window.document from WindowProxy", () => window.document === document);
add("requestIdleCallback from WindowProxy", () => {
  try { window.requestIdleCallback(() => {}); return true; }
  catch (e) { return String(e.message); }
});
add("window.indexedDB from WindowProxy", () => {
  try { return typeof window.indexedDB === "object"; }
  catch (e) { return String(e.message); }
});
add("EventTarget on WindowProxy", () => {
  let seen = 0;
  window.addEventListener("feature-receiver", () => { seen += 1; });
  window.dispatchEvent(new Event("feature-receiver"));
  return seen === 1;
});
add("setTimeout from WindowProxy", () => typeof window.setTimeout === "function");
add("matchMedia from WindowProxy", () => window.matchMedia("(min-width: 1px)").matches === true);
add("getComputedStyle from WindowProxy", () =>
    typeof getComputedStyle.call(window, document.body).display === "string");
''')

page("030_node_and_image_receiver", r'''
document.body.innerHTML = "<div id=target></div>";
add("Node member through WindowProxy realm", () => {
  const child = document.getElementById("target");
  return Node.prototype.contains.call(document, child) === true;
});
add("HTMLImageElement members through WindowProxy realm", () => {
  const image = document.createElement("img");
  image.src = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==";
  return image.src.indexOf("data:") === 0 && image.naturalWidth === 0;
});
''')

page("031_callback_exception_does_not_fail_advance", r'''
let ran = 0;
setTimeout(() => { ran += 1; throw new Error("page callback error"); }, 5);
setTimeout(() => {
  add("callback ran", () => ran === 1);
  R.done = true;
  R.pass = R.cases.every(c => c.pass);
}, 20);
''')

page("027_eval_global_declaration", r'''
const name = "_0c2b88b61aa6e9a834e3a0cb";
const code = "function " + name + "(s){return function(k){for(var o=new Uint8Array(k.length),i=0;i<k.length;i++)o[i]=k[i]^s.charCodeAt(i%s.length);return o}}";
const indirectEval = eval;
const before = typeof window[name];
const returned = indirectEval.call(window, code);
const factory = window[name];
let decoded = null;
if (typeof factory === "function") {
  decoded = Array.from(factory("ab")([1, 2, 3]));
}
add("global function is undefined before eval", () => before === "undefined");
add("eval.call(window, code) returns undefined", () => returned === undefined);
add("declaration is installed on window", () => typeof factory === "function");
add("installed function is an own window property", () =>
    Object.prototype.hasOwnProperty.call(window, name));
add("installed function name matches declaration", () => factory.name === name);
add("installed function decodes the XOR sample", () =>
    Array.isArray(decoded) && decoded.join(",") === "96,96,98");
''')

print("generated", len(list(ROOT.glob("*.html"))), "HTML files")

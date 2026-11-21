#include "runtime/runtime.h"
#include "runtime/page_reference.h"
#include "runtime/exception_capture.h"
#include "execution_scheduling/realm.h"
#include "browser_environment/cross_origin.h"
#include "jsapi.h"
#include "js/CharacterEncoding.h"
#include "js/CallAndConstruct.h"
#include "js/CompilationAndEvaluation.h"
#include "js/Exception.h"
#include <cstring>
#include <string>
#include "js/GlobalObject.h"
#include "js/PropertyAndElement.h"
#include "jsapi.h"
#include "js/Initialization.h"
#include "js/Object.h"
#include "js/Realm.h"
#include "js/RealmOptions.h"
#include "js/SourceText.h"
#include "js/StableStringChars.h"
#include "js/Proxy.h"
#include "js/Wrapper.h"
#include "js/friend/WindowProxy.h"
#include "vm/StandaloneTrace.h"

namespace ruyijs {
namespace {
PageFrames* FramesOf(JSObject* page) {
  const JS::Value value = JS::GetReservedSlot(page, JSCLASS_GLOBAL_SLOT_COUNT + unsigned(PageReference::Frames));
  return value.isUndefined() ? nullptr : static_cast<PageFrames*>(value.toPrivate());
}
// Window indexed properties: the standard-class lazy resolution first, then an
// array index below the child count resolves to that child's WindowProxy
// (enumerable, configurable, read-only, like the donor's WindowProxy).
bool PageResolve(JSContext* cx, JS::HandleObject page, JS::HandleId id, bool* resolved) {
  if (!JS_ResolveStandardClass(cx, page, id, resolved)) return false;
  if (*resolved || !id.isInt() || id.toInt() < 0) return true;
  PageFrames* frames = FramesOf(page);
  const size_t index = size_t(id.toInt());
  if (!frames || !frames->count || index >= frames->count(frames->context)) return true;
  JS::RootedValue child(cx);
  if (!frames->at(frames->context, index, &child)) return false;
  if (child.isUndefined()) return true;
  if (!JS_WrapValue(cx, &child) || !JS_DefinePropertyById(cx, page, id, child, JSPROP_ENUMERATE | JSPROP_READONLY))
    return false;
  frames->resolved = std::max(frames->resolved, index + 1);
  *resolved = true;
  return true;
}
bool PageMayResolve(const JSAtomState& names, jsid id, JSObject* maybe) {
  return id.isInt() || JS_MayResolveStandardClass(names, id, maybe);
}
// Own keys list the child indices first (Firefox: indexed properties precede
// the named ones), then the standard classes.
// ruyitrace (Firefox 151) resolves standard classes lazily; its
// Object.getOwnPropertyNames(window) head is the std-class table order.
// ruyijs (159) calls InitRealmStandardClasses, so all std classes are already
// resolved and the native enumeration would put Function/Object/eval/... first.
// Append the rt head here: newEnumerate results are deduplicated against the
// native enumeration, so these names keep their position. DOM properties after
// the std block still follow their own installation order (known limit).
static const char* const kRtOwnKeysHead[] = {
    "undefined", "Boolean", "JSON", "Date", "Math", "RegExp",
    "InternalError", "AggregateError"};

bool PageEnumerate(JSContext* cx, JS::HandleObject page, JS::MutableHandleIdVector properties, bool enumerableOnly) {
  if (!enumerableOnly) {
    for (const char* name : kRtOwnKeysHead) {
      JS::RootedString atom(cx, JS_AtomizeAndPinString(cx, name));
      if (!atom || !properties.append(JS::PropertyKey::fromPinnedString(atom)))
        return false;
    }
  }
  if (PageFrames* frames = FramesOf(page); frames && frames->count) {
    const size_t count = frames->count(frames->context);
    // Resolving each index defines it as an own property, which the native
    // enumeration then lists.
    JS::RootedId index(cx);
    for (size_t i = 0; i < count; ++i) {
      bool found = false;
      index = JS::PropertyKey::Int(int32_t(i));
      if (!JS_HasOwnPropertyById(cx, page, index, &found)) return false;
    }
  }
  return JS_NewEnumerateStandardClasses(cx, page, properties, enumerableOnly);
}
const JSClassOps kPageOps = {nullptr, nullptr, nullptr, PageEnumerate, PageResolve, PageMayResolve,
                             nullptr, nullptr, nullptr, JS_GlobalObjectTraceHook};

// The page global is exposed to script through an opaque Proxy (WindowProxy).
// Keep it in its own handler family so CheckedUnwrapStatic does not unwrap it:
// structuredClone would otherwise see the bare global instead of a Window.
// Native DOM brand checks unwrap via UnwrapPageProxy (page_helpers.h).
static const char kPageWindowProxyFamily = 0;
class PageWindowProxyHandler final : public js::ForwardingProxyHandler {
 public:
  constexpr PageWindowProxyHandler()
      : js::ForwardingProxyHandler(&kPageWindowProxyFamily) {}
};
static const PageWindowProxyHandler kPageWindowProxyHandler;

static JSObject* NewPageWindowProxy(JSContext* cx, JS::HandleObject global) {
  JSAutoRealm realm(cx, global);
  js::WrapperOptions options;
  if (const JSClass* clasp = js::standalone_trace::RuyijsWindowProxyClass(cx))
    options.setClass(clasp);
  JSObject* proxy = js::Wrapper::New(cx, global, &js::Wrapper::singleton, options);
  return proxy;
}
}  // namespace
static const JSClass kPageClass = {"ruyijsPage", JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(unsigned(PageReference::Count)),
                                   &kPageOps};
void SetPageFrames(JS::HandleObject page, PageFrames* frames) {
  if (!page || JS::GetClass(page) != &kPageClass) return;
  JS::SetReservedSlot(page, JSCLASS_GLOBAL_SLOT_COUNT + unsigned(PageReference::Frames),
                      frames ? JS::PrivateValue(frames) : JS::UndefinedValue());
}
bool InvalidatePageFrames(JSContext* cx, JS::HandleObject page) {
  if (!page || JS::GetClass(page) != &kPageClass) return true;
  PageFrames* frames = FramesOf(page);
  if (!frames || !frames->resolved) return true;
  JSAutoRealm realm(cx, page);
  for (size_t i = 0; i < frames->resolved; ++i) {
    JS::ObjectOpResult removed;
    if (!JS_DeleteElement(cx, page, uint32_t(i), removed)) return false;
  }
  frames->resolved = 0;
  return true;
}

bool ReadPageReference(JS::HandleObject page, PageReference slot, JS::MutableHandleValue value) {
  if (!page || JS::GetClass(page) != &kPageClass || slot >= PageReference::Count) return false;
  value.set(JS::GetReservedSlot(page, JSCLASS_GLOBAL_SLOT_COUNT + unsigned(slot)));
  return true;
}
bool WritePageReference(JS::HandleObject page, PageReference slot, JS::HandleValue value) {
  if (!page || JS::GetClass(page) != &kPageClass || slot >= PageReference::Count) return false;
  JS::SetReservedSlot(page, JSCLASS_GLOBAL_SLOT_COUNT + unsigned(slot), value);
  return true;
}

// The challenge calls indirect eval with a 156-byte global function
// declaration for its XOR decoder factory. The reference engine installs the
// declaration on the page global; the standalone donor evaluates it without
// creating the global binding. Preserve native eval and mirror just that
// declaration shape by evaluating the declaration expression and installing
// the resulting function on the current global.
static bool EvalGlobalFix(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject callee(cx, &args.callee());
  JS::RootedObject global(cx, JS::CurrentGlobalOrNull(cx));
  if (args.thisv().isObject()) global = &args.thisv().toObject();
  if (!callee || !global) return false;
  JS::RootedValue original(cx);
  if (!JS_GetProperty(cx, callee, "__ruyijsOriginalEval", &original) ||
      !original.isObject()) {
    return false;
  }
  JS::RootedValue result(cx);
  if (!JS_CallFunctionValue(cx, global, original,
                            JS::HandleValueArray::fromMarkedLocation(argc, args.array()),
                            &result)) {
    return false;
  }
  if (argc > 0) {
    JS::RootedValue sourceValue(cx, args[0]);
    if (sourceValue.isObject()) {
      JS::RootedObject sourceObject(cx, &sourceValue.toObject());
      JS::RootedValue toString(cx);
      JS::RootedValue converted(cx);
      if (JS_GetProperty(cx, sourceObject, "toString", &toString) &&
          toString.isObject() &&
          JS_CallFunctionValue(cx, sourceObject, toString,
                               JS::HandleValueArray::empty(), &converted) &&
          converted.isString()) {
        sourceValue.set(converted);
      } else {
        JS_ClearPendingException(cx);
      }
    }
    JS::RootedString text(cx, sourceValue.isString() ? sourceValue.toString() : nullptr);
    // The narrowed repair only targets the 156-byte XOR decoder factory.
    // Do not copy/encode the multi-megabyte VM sources that CF evals in a
    // loop; that work is pure overhead and delays the page watchdog.
    JS::UniqueChars utf8 = (text && JS_GetStringLength(text) <= 4096)
                               ? JS_EncodeStringToUTF8(cx, text) : nullptr;
    if (utf8) {
      std::string source(utf8.get());
      if (source.rfind("function ", 0) == 0 &&
          source.find("charCodeAt") != std::string::npos &&
          source.find("Uint8Array") != std::string::npos) {
        const size_t paren = source.find('(');
        if (paren != std::string::npos && paren > 9) {
          const std::string name = source.substr(9, paren - 9);
          if (!name.empty()) {
            JS::RootedValue fn(cx);
            const std::string wrapped = "(" + source + ")";
            JS::SourceText<mozilla::Utf8Unit> sourceText;
            JS::CompileOptions options(cx);
            if (sourceText.init(cx, wrapped.c_str(), wrapped.size(),
                                JS::SourceOwnership::Borrowed) &&
                JS::Evaluate(cx, options, sourceText, &fn) && fn.isObject()) {
              JS::RootedString nameString(cx, JS_AtomizeAndPinString(cx, name.c_str()));
              JS::RootedId id(cx, JS::PropertyKey::fromPinnedString(nameString));
              if (!JS_DefinePropertyById(cx, global, id, fn, JSPROP_ENUMERATE))
                return false;
            } else {
              JS_ClearPendingException(cx);
            }
          }
        }
      }
    }
  }
  args.rval().set(result);
  return true;
}

// Chromium's V8/Blink integration installs SetWasmStreamingCallback. The
// embedder accepts a Response, validates it, then feeds its body to V8. The
// standalone shell's consume callback only accepts BufferSource or
// StreamCacheEntryObject, so wasm-bindgen's `instantiateStreaming(Response)`
// throws synchronously and never reaches its Promise fallback. Install the
// same browser-side Response handling on the page's WebAssembly namespace.
static bool InstallWasmResponseStreaming(JSContext* cx, JS::HandleObject page) {
  JSAutoRealm realm(cx, page);
  JS::RootedValue wasm(cx);
  if (!JS_GetProperty(cx, page, "WebAssembly", &wasm)) return false;
  if (!wasm.isObject()) return true;
  static const char source[] = R"js((function() {
  const wasm = WebAssembly;
  if (!wasm || typeof wasm.instantiate !== "function" ||
      typeof wasm.instantiateStreaming !== "function") return;
  const descriptor = Object.getOwnPropertyDescriptor(wasm, "instantiateStreaming");
  if (!descriptor || !descriptor.configurable) return;
  const original = descriptor.value;
  if (typeof original !== "function") return;

  function isResponse(value) {
    return !!value && typeof value === "object" &&
        typeof value.then !== "function" &&
        typeof value.arrayBuffer === "function";
  }
  function isPromise(value) {
    return !!value && typeof value === "object" && typeof value.then === "function";
  }
  function checkResponse(response) {
    if (!isResponse(response)) {
      throw new TypeError("Argument 1 must be a Response or Promise<Response> object");
    }
    if (response.ok === false ||
        (typeof response.status === "number" &&
         (response.status < 200 || response.status > 299))) {
      throw new TypeError("HTTP status code is not ok");
    }
    const headers = response.headers;
    const mime = headers && typeof headers.get === "function"
        ? headers.get("Content-Type") : null;
    if (typeof mime !== "string" || mime.toLowerCase() !== "application/wasm") {
      throw new TypeError("Incorrect response MIME type. Expected 'application/wasm'.");
    }
    if (response.bodyUsed || (response.body && response.body.locked)) {
      throw new TypeError("Cannot compile WebAssembly.Module from an already read Response");
    }
  }

  function instantiateStreaming(source, imports, options) {
    if (isResponse(source) || isPromise(source)) {
      const hasOptions = arguments.length > 2;
      return Promise.resolve(source).then(function(response) {
        checkResponse(response);
        return response.arrayBuffer().then(function(bytes) {
          if (!bytes || bytes.byteLength === 0) {
            throw new WebAssembly.CompileError("Empty WebAssembly module");
          }
          return hasOptions
              ? WebAssembly.instantiate(bytes, imports, options)
              : WebAssembly.instantiate(bytes, imports);
        });
      });
    }
    return Reflect.apply(original, wasm, arguments);
  }
  Object.defineProperty(instantiateStreaming, "length",
                        {value: original.length, configurable: true});
  Object.defineProperty(instantiateStreaming, "name",
                        {value: original.name, configurable: true});
  descriptor.value = instantiateStreaming;
  Object.defineProperty(wasm, "instantiateStreaming", descriptor);
})();
)js";
  JS::SourceText<mozilla::Utf8Unit> sourceText;
  JS::CompileOptions options(cx);
  options.setFileAndLine("ruyijs-wasm-streaming.js", 1);
  JS::RootedValue result(cx);
  return sourceText.init(cx, source, std::strlen(source),
                         JS::SourceOwnership::Borrowed) &&
         JS::Evaluate(cx, options, sourceText, &result);
}

JSObject* CreatePage(JSContext* cx, JS::HandleObject sameOrigin, bool withWindowProxy) {
  JS::RealmOptions options;
  if (sameOrigin) options.creationOptions().setExistingCompartment(sameOrigin);
  else options.creationOptions().setNewCompartmentAndZone();
  // As the donor's window globals (XPCJSContext SetPrefableRealmOptions with
  // javascript.options.shared_memory on; nsGlobalWindowOuter without
  // cross-origin isolation): Atomics exists, the SharedArrayBuffer global
  // constructor is not defined.
  options.creationOptions().setSharedMemoryAndAtomicsEnabled(true)
      .setDefineSharedArrayBufferConstructor(false);
  options.behaviors().setReduceTimerPrecisionCallerType({0});
  JS::RootedObject page(cx, JS_NewGlobalObject(cx, &kPageClass, nullptr,
      JS::DontFireOnNewGlobalHook, options));
  if (!page) return nullptr;
  {
    JSAutoRealm realm(cx, page);
    if (withWindowProxy) {
      JS::RootedObject proxy(cx, NewPageWindowProxy(cx, page));
      if (!proxy) return nullptr;
      js::standalone_trace::SetRuyijsWindowProxy(cx, page, proxy);
    }
    if (!JS::InitRealmStandardClasses(cx)) return nullptr;
    if (!InstallWasmResponseStreaming(cx, page)) return nullptr;
    {
      JS::RootedValue original(cx);
      if (!JS_GetProperty(cx, page, "eval", &original) || !original.isObject())
        return nullptr;
      JSFunction* wrapper = JS_DefineFunction(cx, page, "eval", EvalGlobalFix, 1, 0);
      if (!wrapper) return nullptr;
      JS::RootedObject wrapperObject(cx, JS_GetFunctionObject(wrapper));
      if (!JS_DefineProperty(cx, wrapperObject, "__ruyijsOriginalEval", original, 0))
        return nullptr;
    }
    JS_FireOnNewGlobalObject(cx, page);
  }
  return page;
}

bool ToUTF8(JSContext* cx, JS::HandleString input, std::string* output) {
  JSLinearString* linear = JS_EnsureLinearString(cx, input);
  if (!linear) return false;
  const size_t size = JS::GetDeflatedUTF8StringLength(linear);
  JS::UniqueChars encoded = JS_EncodeStringToUTF8(cx, input);
  if (!encoded) return false;
  output->assign(encoded.get(), size);
  return true;
}

bool Evaluate(JSContext* cx, JS::HandleObject page, JS::HandleString code,
              const char* filename, Checkpoint checkpoint,
              JS::MutableHandleValue value, bool* success, const EvalOptions& evalOptions,
              const char* introduction, bool host) {
  execution_scheduling::BeginPageOperation(cx, page);
  JS::AutoStableStringChars characters(cx);
  if (!characters.initTwoByte(cx, code)) return false;
  {
    JSAutoRealm realm(cx, page);
    JS::CompileOptions options(cx);
    // A script run synchronously for a page script (a DOM-inserted inline
    // script) names that caller, as the donor ScriptLoader does: the stack
    // then reads "<url> line <n> > injectedScript".
    JS::RootedScript introducer(cx);
    if (introduction) options.setIntroductionInfoToCaller(cx, introduction, &introducer);
    options.setFileAndLine(filename, 1);
    // Public ruyijs applies offsets only when BOTH values are nonnegative.
    if (evalOptions.line() >= 0 && evalOptions.col() >= 0) {
      options.setFileAndLine(filename, uint32_t(evalOptions.line()) + 1);
      options.setColumn(JS::ColumnNumberOneOrigin(uint32_t(evalOptions.col()) + 1));
    }
    JS::SourceText<char16_t> source;
    if (!source.init(cx, characters.twoByteChars(), characters.length(),
                     JS::SourceOwnership::Borrowed))
      return false;
    JS::RootedScript script(cx, JS::Compile(cx, options, source));
    if (script && host) browser_environment::MarkHostScript(script);
    *success = script && JS_ExecuteScript(cx, script, value);
    if (!*success) {
      CaptureOperationFailure(cx);
      value.setUndefined();
    }
    checkpoint(cx);
    if (JS_IsExceptionPending(cx)) {
      *success = false;
      CaptureOperationFailure(cx);
    }
  }
  return JS_WrapValue(cx, value);
}
bool EvaluateHostExpression(JSContext* cx, const char* filename, const char16_t* chars, size_t length,
                            JS::MutableHandleValue value) {
  JS::CompileOptions options(cx);
  options.setFileAndLine(filename, 1);
  JS::SourceText<char16_t> source;
  if (!source.init(cx, chars, length, JS::SourceOwnership::Borrowed)) return false;
  JS::RootedScript script(cx, JS::Compile(cx, options, source));
  if (!script) return false;
  browser_environment::MarkHostScript(script);
  return JS_ExecuteScript(cx, script, value);
}
}

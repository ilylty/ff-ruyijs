#include "runtime/cf_access_trace.h"

#include "js/CallAndConstruct.h"
#include "js/Class.h"
#include "js/Object.h"
#include "js/RootingAPI.h"
#include "js/String.h"
#include "js/Symbol.h"
#include "js/Value.h"
#include "jsapi.h"
#include "runtime/cf_access_log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace ruyijs {
namespace {

void AppendJson(std::string* out, const std::string& text) {
  cf_log::append_json_string(*out, text.c_str());
}

std::string ObjectId(JSObject* object) {
  char buffer[24];
  const unsigned long long bits =
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(object)) &
      0xFFFFFFFFFFFFULL;
  std::snprintf(buffer, sizeof(buffer), "%012llx", bits);
  return std::string(buffer);
}

// The class name a page can observe, not the engine's internal JSClass.name.
// ruyijsPage is a C++ implementation detail; Object.prototype.toString and
// Symbol.toStringTag both report "Window" for the global on both engines.
// Reading the tag through the JS-visible path keeps the trace aligned with
// what a challenge script can actually see.
std::string ObjectClass(JSContext* cx, JS::HandleObject object) {
  JS::RootedValue tag(cx);
  JS::RootedId tagId(cx, JS::PropertyKey::Symbol(
      JS::GetWellKnownSymbol(cx, JS::SymbolCode::toStringTag)));
  if (JS_GetPropertyById(cx, object, tagId, &tag) && tag.isString()) {
    JS::RootedString text(cx, tag.toString());
    JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, text);
    if (utf8 && *utf8.get()) return std::string(utf8.get());
  }
  JS_ClearPendingException(cx);
  JS::RootedValue toString(cx);
  if (!JS_GetProperty(cx, object, "toString", &toString) ||
      !toString.isObject() || !JS::IsCallable(&toString.toObject())) {
    JS_ClearPendingException(cx);
    return "Object";
  }
  JS::RootedValue result(cx);
  JS::RootedValue function(cx, toString);
  if (!JS_CallFunctionValue(cx, object, function, JS::HandleValueArray::empty(),
                            &result) ||
      !result.isString()) {
    JS_ClearPendingException(cx);
    return "Object";
  }
  JS::RootedString text(cx, result.toString());
  JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, text);
  if (!utf8) {
    JS_ClearPendingException(cx);
    return "Object";
  }
  std::string repr(utf8.get());
  if (repr.size() > 9 && repr.compare(0, 8, "[object ") == 0 &&
      repr.back() == ']') {
    return repr.substr(8, repr.size() - 9);
  }
  return "Object";
}

bool IsDomObject(JSObject* object) {
  const JSClass* cls = JS::GetClass(object);
  return cls && (cls->flags & JSCLASS_IS_DOMJSCLASS);
}

// ruyitrace writes the value read/written as a JSON scalar; objects collapse to
// "[Object Class]" and DOM objects to "[object]" (enumerating them at the
// funnel can hang). Functions carry their name when the value is a constructor.
std::string ValueText(JSContext* cx, JS::HandleValue value) {
  if (value.isUndefined()) return "\"undefined\"";
  if (value.isNull()) return "\"null\"";
  if (value.isBoolean()) return value.toBoolean() ? "true" : "false";
  if (value.isInt32()) return std::to_string(value.toInt32());
  if (value.isNumber()) {
    const double number = value.toNumber();
    if (std::isnan(number)) return "\"NaN\"";
    if (std::isinf(number)) return number > 0 ? "\"Infinity\"" : "\"-Infinity\"";
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", number);
    return std::string(buffer);
  }
  if (value.isString()) {
    JS::RootedString text(cx, value.toString());
    JS::UniqueChars utf8 = text ? JS_EncodeStringToUTF8(cx, text) : nullptr;
    if (!utf8) {
      JS_ClearPendingException(cx);
      return "\"\"";
    }
    const size_t length = std::strlen(utf8.get());
    const size_t kept = length > 200 ? 200 : length;
    std::string out;
    cf_log::append_json_string(out, std::string(utf8.get(), kept).c_str());
    return out;
  }
  if (value.isObject()) {
    JS::RootedObject object(cx, &value.toObject());
    if (IsDomObject(object)) return "\"[object]\"";
    if (JS_ObjectIsFunction(object)) {
      JS::RootedValue name(cx);
      if (JS_GetProperty(cx, object, "name", &name) && name.isString()) {
        JS::RootedString text(cx, name.toString());
        JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, text);
        if (utf8 && *utf8.get()) {
          std::string out = "{\"type\":\"Function\",\"name\":";
          AppendJson(&out, utf8.get());
          out += "}";
          return out;
        }
      }
      JS_ClearPendingException(cx);
      return "{\"type\":\"Function\",\"name\":\"\"}";
    }
    // L2: never emit an opaque [Object Class] token. Keep a stable repr,
    // a payload hash and byte_length so the value is auditable.
    const std::string repr = std::string("[Object ") +
                            ObjectClass(cx, object) + "]";
    const std::string payload = repr + "|" + ObjectId(object);
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : payload) {
      hash ^= c;
      hash *= 1099511628211ull;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(hash));
    std::string out = "{\"repr\":";
    AppendJson(&out, repr);
    out += ",\"payload_hash\":";
    AppendJson(&out, hex);
    out += ",\"hash_algo\":\"fnv1a64\"";
    out += ",\"byte_length\":" +
            std::to_string(payload.size()) +
            ",\"truncated\":true}";
    return out;
  }
  return "\"[object]\"";
}

std::string ObjectText(JSContext* cx, JS::HandleValue value) {
  if (!value.isObject()) return "null";
  JS::RootedObject object(cx, &value.toObject());
  std::string out = "{\"id\":";
  AppendJson(&out, ObjectId(object));
  out += ",\"class\":";
  AppendJson(&out, ObjectClass(cx, object));
  out += "}";
  return out;
}

std::string ArgsText(JSContext* cx, const JS::CallArgs& args) {
  std::string out = "[";
  const unsigned count = args.length() < 16 ? args.length() : 16;
  for (unsigned i = 0; i < count; ++i) {
    if (i) out += ",";
    JS::RootedValue value(cx, args[i]);
    out += ValueText(cx, value);
  }
  out += "]";
  return out;
}

}  // namespace

CfAccessTrace::CfAccessTrace(JSContext* cx, const JS::CallArgs& args,
                             const char* iface, const char* member, int type,
                             bool enabled)
    : cx_(cx), args_(args), iface_(iface ? iface : ""),
      member_(member ? member : ""), type_(type), enabled_(enabled) {}

CfAccessTrace::~CfAccessTrace() {
  if (!enabled_) return;
  // Read the finished native's values. The CallArgs wraps the caller's vp, which
  // stays alive until the native returns; the destructor runs just before that.
  JS::RootedValue result(cx_, args_.rval());
  JS::RootedValue receiver(cx_, args_.thisv());
  const bool ok = ok_ && !JS_IsExceptionPending(cx_);
  const std::string valueText = ValueText(cx_, result);
  const std::string returnText = ok ? valueText : std::string("null");
  const std::string objectText = ObjectText(cx_, result);
  cf_log::observe_scoped(
      iface_.c_str(), member_.c_str(), type_,
      valueText.c_str(), objectText.c_str(),
      returnText.c_str(), ok ? objectText.c_str() : "null",
      ObjectText(cx_, receiver).c_str(),
      ArgsText(cx_, args_).c_str(), nullptr);
}

}  // namespace ruyijs

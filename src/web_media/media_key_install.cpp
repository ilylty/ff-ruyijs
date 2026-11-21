#include "web_media/media_key_internal.h"
#include "runtime/page_helpers.h"
#include "js/Object.h"
#include "js/Conversions.h"
#include "js/PropertyAndElement.h"
#include "runtime/exception_capture.h"
#include "js/Exception.h"
#include "js/Wrapper.h"

namespace ruyijs::web_media {
namespace {
bool NavigatorAccess(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp); auto& state = *static_cast<State*>(WebKit::Owner(args));
  JSObject* receiver = args.thisv().isObject() ? js::CheckedUnwrapStatic(&args.thisv().toObject()) : nullptr;
  const bool compatible = receiver && receiver == state.navigator;
  // PRO keeps the WebIDL brand-check TypeError pending but returns a
  // successful native call. Capture and clear it while resolving the promise,
  // then restore it for the host boundary after the native call succeeds.
  if (!compatible) {
    (void)WebKit::Fail(cx, "Navigator receiver is incompatible");
    CaptureHostBoundaryFailure(cx);
  }
  if (argc < 2) return WebKit::Fail(cx, "requestMediaKeySystemAccess requires keySystem and configurations");
  JS::RootedString key(cx, JS::ToString(cx, args[0])); if (!key) return false;
  JS::RootedObject config(cx, args[1].isObject() ? &args[1].toObject() : nullptr);
  if (!config) return WebKit::Fail(cx, "supportedConfigurations must be an object");
  JS::RootedObject access(cx, state.kit.New(&kMediaKeySystemAccessClass, state.keySystemAccess.prototype));
  if (!access) return false;
  JS::SetReservedSlot(access, AccessKeySystem, JS::StringValue(key));
  JS::SetReservedSlot(access, AccessConfiguration, JS::ObjectValue(*config));
  JS::RootedValue value(cx, JS::ObjectValue(*access));
  if (!PromiseValue(cx, value, args.rval())) return false;
  return true;
}
}

bool InstallMediaKeys(State& state) {
  if (!InstallMediaKeyStatusMap(state) || !InstallMediaKeySession(state) ||
      !InstallMediaKeySystemAccess(state) || !InstallMediaKeysObject(state) ||
      !InstallMediaKeyError(state)) return false;
  JS::RootedObject prototype(state.cx, state.environment.navigatorPrototype());
  JS::RootedObject method(state.cx, state.kit.Function(NavigatorAccess,
                                                        "requestMediaKeySystemAccess", 2, 0));
  return prototype && method &&
      JS_DefineProperty(state.cx, prototype, "requestMediaKeySystemAccess", method, JSPROP_ENUMERATE) &&
      state.kit.helpers->Member(method, "Navigator", "requestMediaKeySystemAccess", "call");
}
}

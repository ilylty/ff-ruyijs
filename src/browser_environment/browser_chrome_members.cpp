#include "browser_environment/browser_chrome.h"
#include "runtime/page_helpers.h"
#include "jsfriendapi.h"
#include "js/Object.h"
#include "js/Wrapper.h"

namespace ruyijs::browser_environment {
namespace {
int32_t Selector(const JS::CallArgs& args) {
  return js::GetFunctionNativeReserved(&args.callee(), 1).toInt32();
}
State* Branded(JSContext* cx, JS::HandleValue value, const JSClass* shape,
               JS::MutableHandleObject object) {
  object.set(value.isObject() ? js::CheckedUnwrapStatic(&value.toObject()) : nullptr);
  if (object && JS::GetClass(object) == shape) {
    auto slot = JS::GetReservedSlot(object, 0);
    if (!slot.isUndefined()) return static_cast<State*>(slot.toPrivate());
  }
  Error(cx, "Browser environment member called on incompatible receiver");
  return nullptr;
}
}
State* WindowGlobalOwner(JSContext* cx, JS::HandleObject object, JS::MutableHandleObject prototype) {
  // The page global is exposed as a Proxy; JS_IsGlobalObject and the owner's
  // `page` comparison both require the bare Window.
  JS::RootedObject window(cx, UnwrapPageProxy(object.get()));
  if (window && JS_IsGlobalObject(window)) {
    // No JSAutoRealm here: JS_GetPrototype is realm-independent, and switching
    // the context would make JS::GetScriptedCallerGlobal in callers such as
    // WindowMessaging resolve to the wrong (target) global.
    if (!JS_GetPrototype(cx, window, prototype)) return nullptr;
    if (prototype && JS::GetClass(prototype) == &kWindowFacadeClass) {
      auto* owner = static_cast<State*>(JS::GetReservedSlot(prototype, 0).toPrivate());
      if (owner && owner->page == window) return owner;
    }
  }
  Error(cx, "Window member called on incompatible receiver");
  return nullptr;
}
State* WindowOwner(JSContext* cx, const JS::CallArgs& args, JS::MutableHandleObject prototype) {
  auto* caller = static_cast<State*>(js::GetFunctionNativeReserved(&args.callee(), 0).toPrivate());
  JS::RootedObject object(cx, args.thisv().isObject() ?
      UnwrapPageProxy(js::CheckedUnwrapStatic(&args.thisv().toObject())) :
      args.thisv().isNullOrUndefined() && caller ? caller->page.get() : nullptr);
  return WindowGlobalOwner(cx, object, prototype);
}
bool ChromeValueGet(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  int32_t slot = Selector(args);
  const JSClass* shape = slot == 0 ? &kBarPropClass : &kUserActivationClass;
  JS::RootedObject self(cx);
  State* owner = Branded(cx, args.thisv(), shape, &self);
  if (!owner) return false;
  auto __p0trace_0 = owner->ObserveScoped(cx, args, shape->name, slot == 0 ? "visible" : slot == 1 ? "hasBeenActive" : "isActive", int(ApiAccess::Read));
  const bool active = slot == 0 || (slot == 1 ? owner->activation.Sticky() :
      owner->activationClock && owner->activation.Transient(
          owner->activationClock(owner->activationClockContext)));
  args.rval().setBoolean(active);
  return true;
}
bool ChromeVisibleSet(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject self(cx);
  State* owner = Branded(cx, args.thisv(), &kBarPropClass, &self);
  if (!owner) return false;
  if (!argc) return Error(cx, "BarProp.visible setter requires a value");
  auto __p0trace_1 = owner->ObserveScoped(cx, args, "BarProp", "visible", int(ApiAccess::Write));
  // WebIDL boolean conversion cannot call user code. Firefox ignores content writes.
  args.rval().setUndefined();
  return true;
}
bool ChromeBarGet(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject prototype(cx);
  State* owner = WindowOwner(cx, args, &prototype);
  if (!owner) return false;
  unsigned slot = unsigned(Selector(args));
  auto __p0trace_2 = owner->ObserveScoped(cx, args, "Window", kBarNames[slot], int(ApiAccess::Read));
  args.rval().set(JS::GetReservedSlot(prototype, slot + 1));
  return JS_WrapValue(cx, args.rval());
}
bool ChromeBarSet(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject prototype(cx);
  State* owner = WindowOwner(cx, args, &prototype);
  if (!owner) return false;
  unsigned slot = unsigned(Selector(args));
  auto __p0trace_3 = owner->ObserveScoped(cx, args, "Window", kBarNames[slot], int(ApiAccess::Write));
  JS::RootedValue value(cx, args.get(0));
  JSAutoRealm realm(cx, owner->page);
  if (!JS_WrapValue(cx, &value) ||
      !JS_DefineProperty(cx, owner->page, kBarNames[slot], value, JSPROP_ENUMERATE)) return false;
  args.rval().setUndefined();
  return true;
}
bool ChromeActivationGet(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject self(cx);
  State* owner = Branded(cx, args.thisv(), ClassFor(Kind::Navigator), &self);
  if (!owner) return false;
  auto __p0trace_4 = owner->ObserveScoped(cx, args, "Navigator", "userActivation", int(ApiAccess::Read));
  args.rval().set(JS::GetReservedSlot(self, kNavigatorActivationSlot));
  return JS_WrapValue(cx, args.rval());
}
}

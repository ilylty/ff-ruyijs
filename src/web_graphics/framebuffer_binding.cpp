// Binding and attachment state for PRO's WebGL renderbuffer/framebuffer calls.
#include "web_graphics/internal.h"
#include "web_graphics/public_calls.h"
#include "web_graphics/draw_calls.h"

#include "js/Conversions.h"
#include "js/PropertyAndElement.h"

#include <cmath>

namespace ruyijs::web_graphics {
namespace {
constexpr int32_t kFramebufferTarget = 0x8D40;   // FRAMEBUFFER
constexpr int32_t kReadFramebuffer = 0x8CA8;
constexpr int32_t kDrawFramebuffer = 0x8CA9;
constexpr int32_t kRenderbufferTarget = 0x8D41;  // RENDERBUFFER
constexpr int32_t kTexture2D = 0x0DE1;
constexpr int32_t kColorAttachment0 = 0x8CE0;
constexpr int32_t kDepthAttachment = 0x8D00;
constexpr int32_t kStencilAttachment = 0x8D20;
constexpr int32_t kDepthStencilAttachment = 0x821A;
constexpr int32_t kProFramebufferComplete = 0x8CD5;
constexpr int32_t kProFramebufferIncompleteMissingAttachment = 0x8CD7;
constexpr int32_t kProMaxSamples = 0x8D57;
constexpr int32_t kProMaxRenderbufferSize = 0x84E8;

bool ValidAttachment(int32_t version, int32_t attachment) {
  if (attachment == kColorAttachment0 || attachment == kDepthAttachment ||
      attachment == kStencilAttachment || attachment == kDepthStencilAttachment)
    return true;
  if (version != 2) return false;
  return attachment > kColorAttachment0 && attachment <= kColorAttachment0 + 15;
}

bool EnumAt(JSContext* cx, const JS::CallArgs& args, unsigned index, int32_t* out) {
  return detail::ToEnum(cx, args.get(index), out);
}

// PRO clamps width/height to the same [-1, 0x10000] integer range before the
// MAX_RENDERBUFFER_SIZE check. NaN becomes zero, matching ToNumber's WebIDL use.
bool NumberAt(JSContext* cx, const JS::CallArgs& args, unsigned index, int32_t* out) {
  double number = 0;
  if (!JS::ToNumber(cx, args.get(index), &number)) return false;
  if (number > 65535.0) *out = 0x10000;
  else if (number < 0.0) *out = -1;
  else if (!std::isfinite(number)) *out = 0;
  else *out = static_cast<int32_t>(number);
  return true;
}

int32_t LimitOf(const Context& context, int32_t pname, int32_t fallback) {
  ParameterStateValue state;
  if (GetParameterState(context, pname, &state) &&
      state.kind == ParameterKind::Number)
    return static_cast<int32_t>(state.values[0]);
  return fallback;
}

JSObject* BoundFramebuffer(Context& context, int32_t target) {
  return target == kReadFramebuffer ? context.readFramebuffer.get()
                                    : context.drawFramebuffer.get();
}
}  // namespace

// The five PRO product calls share one arity/error/ownership path. The public
// method table's op decides which WebGL state the call writes.
bool RenderbufferStateCall(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  State& state = *detail::Self(args);
  const int op = WebKit::Slot(args) & 0xffff;
  JS::RootedObject self(cx);
  if (!detail::ContextReceiver(cx, args, &self)) return false;
  unsigned required = 0;
  switch (op) {
    case kBindRenderbuffer: required = 2; break;
    case kFramebufferRenderbuffer: case kRenderbufferStorage: required = 4; break;
    case kFramebufferTexture2D: case kRenderbufferStorageMultisample: required = 5; break;
    default: return true;
  }
  if (args.length() < required)
    return WebKit::Fail(cx, "WebGL method requires more arguments");
  Context& context = *detail::ContextOf(self);
  args.rval().setUndefined();

  switch (op) {
    case kBindRenderbuffer: {
      int32_t target = 0;
      if (!EnumAt(cx, args, 0, &target)) return false;
      if (target != kRenderbufferTarget) {
        detail::SetError(context, kInvalidEnum);
        return true;
      }
      JS::HandleValue object = args.get(1);
      if (object.isNullOrUndefined()) {
        context.renderbufferBinding = nullptr;
        return true;
      }
      JSObject* owned = draw_call_detail::Owned(state, context, object,
                                               kRenderbufferObject);
      if (!owned) {
        detail::SetError(context, kInvalidOperation);
        return true;
      }
      context.renderbufferBinding = owned;
      return true;
    }
    case kFramebufferRenderbuffer: {
      int32_t target = 0, attachment = 0, renderbufferTarget = 0;
      if (!EnumAt(cx, args, 0, &target) ||
          !EnumAt(cx, args, 1, &attachment) ||
          !EnumAt(cx, args, 2, &renderbufferTarget)) return false;
      const bool targetValid = target == kFramebufferTarget ||
          (context.version == 2 && (target == kReadFramebuffer || target == kDrawFramebuffer));
      if (!targetValid || !ValidAttachment(context.version, attachment) ||
          renderbufferTarget != kRenderbufferTarget) {
        detail::SetError(context, kInvalidEnum);
        return true;
      }
      JS::RootedObject framebuffer(cx, BoundFramebuffer(context, target));
      if (!framebuffer) {
        detail::SetError(context, kInvalidOperation);
        return true;
      }
      JS::HandleValue object = args.get(3);
      JSObject* renderbuffer = nullptr;
      if (!object.isNullOrUndefined()) {
        renderbuffer = draw_call_detail::Owned(state, context, object,
                                               kRenderbufferObject);
        if (!renderbuffer) {
          detail::SetError(context, kInvalidOperation);
          return true;
        }
      }
      JS::RootedValue value(cx);
      if (renderbuffer) value.setObject(*renderbuffer);
      else value.setNull();
      if (!JS_SetElement(cx, framebuffer, attachment, value)) return false;
      return true;
    }
    case kFramebufferTexture2D: {
      int32_t target = 0, attachment = 0, textureTarget = 0, level = 0;
      if (!EnumAt(cx, args, 0, &target) ||
          !EnumAt(cx, args, 1, &attachment) ||
          !EnumAt(cx, args, 2, &textureTarget)) return false;
      if (!JS::ToInt32(cx, args.get(4), &level)) return false;
      const bool targetValid = target == kFramebufferTarget ||
          (context.version == 2 && (target == kReadFramebuffer || target == kDrawFramebuffer));
      if (!targetValid || !ValidAttachment(context.version, attachment) ||
          textureTarget != kTexture2D || level < 0) {
        detail::SetError(context, kInvalidEnum);
        return true;
      }
      JS::RootedObject framebuffer(cx, BoundFramebuffer(context, target));
      if (!framebuffer) {
        detail::SetError(context, kInvalidOperation);
        return true;
      }
      JS::HandleValue object = args.get(3);
      JSObject* texture = nullptr;
      if (!object.isNullOrUndefined()) {
        texture = draw_call_detail::Owned(state, context, object,
                                          kTextureObject);
        if (!texture) {
          detail::SetError(context, kInvalidOperation);
          return true;
        }
      }
      JS::RootedValue value(cx);
      if (texture) value.setObject(*texture);
      else value.setNull();
      if (!JS_SetElement(cx, framebuffer, attachment, value)) return false;
      return true;
    }
    case kRenderbufferStorage: {
      int32_t target = 0, format = 0, width = 0, height = 0;
      if (!EnumAt(cx, args, 0, &target) || !EnumAt(cx, args, 1, &format) ||
          !NumberAt(cx, args, 2, &width) || !NumberAt(cx, args, 3, &height))
        return false;
      if (target != kRenderbufferTarget) {
        detail::SetError(context, kInvalidEnum);
        return true;
      }
      JSObject* binding = context.renderbufferBinding.get();
      if (!binding) {
        detail::SetError(context, kInvalidOperation);
        return true;
      }
      if (!RenderbufferFormatSupported(state, context, format)) {
        detail::SetError(context, kInvalidEnum);
        return true;
      }
      const int32_t limit = LimitOf(context, kProMaxRenderbufferSize, 16384);
      if (width < 0 || height < 0 || width > limit || height > limit) {
        detail::SetError(context, kInvalidValue);
        return true;
      }
      JS::SetReservedSlot(binding, detail::kObjectDataSlot, JS::Int32Value(format));
      JS::SetReservedSlot(binding, detail::kObjectExtraSlot, JS::Int32Value(0));
      return true;
    }
    case kRenderbufferStorageMultisample: {
      int32_t target = 0, samples = 0, format = 0, width = 0, height = 0;
      if (!EnumAt(cx, args, 0, &target)) return false;
      if (!JS::ToInt32(cx, args.get(1), &samples)) return false;
      if (!EnumAt(cx, args, 2, &format) ||
          !NumberAt(cx, args, 3, &width) || !NumberAt(cx, args, 4, &height))
        return false;
      if (target != kRenderbufferTarget) {
        detail::SetError(context, kInvalidEnum);
        return true;
      }
      JSObject* binding = context.renderbufferBinding.get();
      if (!binding) {
        detail::SetError(context, kInvalidOperation);
        return true;
      }
      if (!RenderbufferFormatSupported(state, context, format)) {
        detail::SetError(context, kInvalidEnum);
        return true;
      }
      const int32_t limit = LimitOf(context, kProMaxRenderbufferSize, 16384);
      const int32_t samplesLimit = LimitOf(context, kProMaxSamples, 4);
      if (samples < 0 || samples > samplesLimit || width < 0 || height < 0 ||
          width > limit || height > limit) {
        detail::SetError(context, kInvalidValue);
        return true;
      }
      JS::SetReservedSlot(binding, detail::kObjectDataSlot, JS::Int32Value(format));
      JS::SetReservedSlot(binding, detail::kObjectExtraSlot, JS::Int32Value(samples));
      return true;
    }
    default:
      return true;
  }
}

bool FramebufferCall(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  State& state = *detail::Self(args);
  const bool bind = (WebKit::Slot(args) & 0xffff) == 1;
  JS::RootedObject self(cx);
  if (!detail::ContextReceiver(cx, args, &self)) return false;
  if (argc < (bind ? 2u : 1u)) return WebKit::Fail(cx, "WebGL method requires more arguments");
  Context& context = *detail::ContextOf(self);
  int32_t target = 0;
  if (!detail::ToEnum(cx, args.get(0), &target)) return false;
  args.rval().setUndefined();
  if (!bind) args.rval().setInt32(0);
  const bool targetValid = target == kFramebufferTarget ||
      (context.version == 2 && (target == kReadFramebuffer || target == kDrawFramebuffer));
  if (!targetValid) {
    detail::SetError(context, kInvalidEnum);
    return true;
  }
  JS::RootedObject framebuffer(cx, BoundFramebuffer(context, target));
  if (!bind) {
    if (!framebuffer) {
      args.rval().setInt32(kProFramebufferComplete);
      return true;
    }
    JS::RootedValue attachment(cx);
    if (!JS_GetElement(cx, framebuffer, kColorAttachment0, &attachment)) return false;
    args.rval().setInt32(attachment.isObject() ? kProFramebufferComplete
                                               : kProFramebufferIncompleteMissingAttachment);
    return true;
  }
  JS::RootedObject object(cx);
  if (!args.get(1).isNullOrUndefined()) {
    object = draw_call_detail::Owned(state, context, args.get(1), kFramebufferObject);
    if (!object) {
      detail::SetError(context, kInvalidOperation);
      return true;
    }
  }
  if (target != kReadFramebuffer) context.drawFramebuffer = object;
  if (target != kDrawFramebuffer) context.readFramebuffer = object;
  return true;
}
}  // namespace ruyijs::web_graphics

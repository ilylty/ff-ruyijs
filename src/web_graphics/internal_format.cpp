// WebGL2RenderingContext.getInternalformatParameter: the PRO build answers the
// RENDERBUFFER / supported-internalformat / SAMPLES query with a descending
// powers-of-two list capped by MAX_SAMPLES. Other combinations set INVALID_ENUM
// and return null. The same format table gates renderbufferStorage calls.
#include "web_graphics/internal.h"
#include "web_graphics/public_calls.h"

#include "js/PropertyAndElement.h"
#include "js/experimental/TypedData.h"

#include <string>

namespace ruyijs::web_graphics {
namespace {
inline constexpr int32_t kRenderbufferTarget = 0x8D41;  // RENDERBUFFER
inline constexpr int32_t kSamplesPname = 0x80A9;        // SAMPLES
inline constexpr int32_t kMaxSamplesPname = 0x8D57;     // MAX_SAMPLES
inline constexpr int32_t kRgbFormat = 0x1907;
inline constexpr int32_t kRgbaFormat = 0x1908;
inline constexpr int32_t kRgb8Format = 0x8051;
inline constexpr int32_t kRgba8Format = 0x8058;
inline constexpr int32_t kStencilIndex8Format = 0x84F9;

// PRO's first format table. The two legacy RGB/RGBA aliases are
// canonicalized before this check. STENCIL_INDEX8 is handled separately.
inline constexpr int32_t kRenderbufferSampleFormats[] = {
    0x8051, 0x8056, 0x8057, 0x8058, 0x8059, 0x81A5, 0x81A6,
    0x8229, 0x822B, 0x8231, 0x8232, 0x8233, 0x8234, 0x8235,
    0x8236, 0x8237, 0x8238, 0x8239, 0x823A, 0x823B, 0x823C,
    0x88F0, 0x8C43, 0x8CAC, 0x8CAD, 0x8D48, 0x8D62, 0x8D70,
    0x8D76, 0x8D7C, 0x8D82, 0x8D88, 0x8D8E, 0x906F,
};

// PRO's second table is gated on getExtension("EXT_color_buffer_float").
inline constexpr int32_t kFloatColorBufferFormats[] = {
    0x822D, 0x822E, 0x822F, 0x8230, 0x8814, 0x881A, 0x8C3A,
};

int32_t CanonicalFormat(int32_t format) {
  if (format == kRgbFormat) return kRgb8Format;
  if (format == kRgbaFormat) return kRgba8Format;
  return format;
}
}  // namespace

bool IsSupportedRenderbufferFormat(int32_t format) noexcept {
  const int32_t canonical = CanonicalFormat(format);
  for (int32_t candidate : kRenderbufferSampleFormats) {
    if (candidate == canonical) return true;
  }
  return false;
}

bool InternalformatEnabled(State& state, Context& context, int32_t format) {
  const int32_t canonical = CanonicalFormat(format);
  for (int32_t candidate : kFloatColorBufferFormats) {
    if (candidate != canonical) continue;
    bool enabled = false;
    return ExtensionEnabled(state, context, "EXT_color_buffer_float", &enabled) &&
           enabled;
  }
  return false;
}

bool RenderbufferFormatSupported(State& state, Context& context, int32_t format) {
  return IsSupportedRenderbufferFormat(format) ||
         InternalformatEnabled(state, context, format);
}

bool GetInternalformatParameter(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  State& state = *detail::Self(args);
  JS::RootedObject self(cx);
  if (!detail::ContextReceiver(cx, args, &self)) return false;
  if (args.length() < 3) {
    std::string message = "WebGL2RenderingContext.getInternalformatParameter: "
                          "At least 3 arguments required, but only ";
    message += std::to_string(args.length());
    message += " passed";
    return WebKit::Fail(cx, message.c_str());
  }
  Context& context = *detail::ContextOf(self);
  int32_t target = 0, format = 0, pname = 0;
  if (!detail::ToEnum(cx, args[0], &target) ||
      !detail::ToEnum(cx, args[1], &format) ||
      !detail::ToEnum(cx, args[2], &pname)) return false;
  format = CanonicalFormat(format);
  // STENCIL_INDEX8 is in PRO's raw format table but produces an empty
  // Int32Array and INVALID_ENUM. Every other unsupported enum returns null.
  if (target == kRenderbufferTarget && pname == kSamplesPname &&
      format == kStencilIndex8Format) {
    JS::RootedObject empty(cx, JS_NewInt32Array(cx, 0));
    if (!empty) return false;
    detail::SetError(context, kInvalidEnum);
    args.rval().setObject(*empty);
    return true;
  }
  const bool supported = RenderbufferFormatSupported(state, context, format);
  if (target != kRenderbufferTarget || pname != kSamplesPname || !supported) {
    detail::SetError(context, kInvalidEnum);
    args.rval().setNull();
    return true;
  }
  double maxSamples = 8;
  ParameterStateValue parameterState;
  if (GetParameterState(context, kMaxSamplesPname, &parameterState) &&
      parameterState.kind == ParameterKind::Number) {
    maxSamples = parameterState.values[0];
  } else if (!state.model.Limit(kMaxSamplesPname, &maxSamples)) {
    JS::RootedValue defaultAnswer(cx);
    bool found = false;
    if (!DefaultParameter(state, context, kMaxSamplesPname, &defaultAnswer, &found))
      return false;
    if (found && defaultAnswer.isNumber()) maxSamples = defaultAnswer.toNumber();
  }
  int32_t count = 0;
  for (int32_t value = static_cast<int32_t>(maxSamples); value > 0; value /= 2) ++count;
  JS::RootedObject result(cx, JS_NewInt32Array(cx, count));
  if (!result) return false;
  int32_t value = static_cast<int32_t>(maxSamples);
  for (int32_t index = 0; index < count; ++index) {
    if (!JS_SetElement(cx, result, index, value)) return false;
    value /= 2;
  }
  args.rval().setObject(*result);
  return true;
}
}  // namespace ruyijs::web_graphics

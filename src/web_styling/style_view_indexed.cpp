// PRO declaration views expose one own indexed property name per declaration.
// The block text remains the authoritative declaration store; these fields are
// refreshed from it so CSSOM reads and cssText / setProperty writes agree.
#include "web_styling/internal.h"
#include "web_styling/page_descriptors.h"
#include "domain/styling/css_declarations.h"
#include "js/Wrapper.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/String.h"

namespace ruyijs::web_styling::detail {
bool RefreshIndexedDeclarationNames(JSContext* cx, JS::HandleObject view,
                                     const std::vector<std::string>& names) {
  JS::RootedObject target(cx, js::CheckedUnwrapStatic(view));
  if (!target || !OwnsDeclaration(target)) return WebKit::Fail(cx, "Invalid CSS declaration receiver");
  // Rule views keep the authoritative declaration block on their Rule, while
  // computed views keep it in slot 0. Refresh the JS-facing wrapper on proxy
  // views and the native storage on both views.
  JS::RootedObject indexedTarget(cx, view);
  JS::RootedObject storage(cx, DeclarationStorage(target));
  if (storage != target) indexedTarget = target;
  uint32_t existing = 0;
  bool matches = true;
  while (matches) {
    JS::RootedId id(cx, JS::PropertyKey::Int(int32_t(existing)));
    bool present = false;
    if (!JS_HasOwnPropertyById(cx, indexedTarget, id, &present)) return false;
    if (!present) break;
    if (existing >= names.size()) { matches = false; break; }
    JS::RootedValue current(cx);
    if (!JS_GetPropertyById(cx, indexedTarget, id, &current)) return false;
    if (!current.isString()) { matches = false; break; }
    JS::RootedString text(cx, current.toString());
    bool same = false;
    if (!AsciiEquals(cx, text, names[existing].c_str(), &same)) return false;
    if (!same) { matches = false; break; }
    ++existing;
  }
  if (matches && existing == names.size()) return true;
  while (true) {
    JS::RootedId id(cx, JS::PropertyKey::Int(int32_t(existing)));
    bool present = false;
    if (!JS_HasOwnPropertyById(cx, indexedTarget, id, &present)) return false;
    if (!present) break;
    JS::ObjectOpResult removed;
    if (!JS_DeleteElement(cx, indexedTarget, existing, removed) || !removed.ok()) return false;
    ++existing;
  }
  for (uint32_t i = 0; i < names.size(); ++i) {
    JS::RootedString name(cx, JS_NewStringCopyUTF8N(cx,
        JS::UTF8Chars(names[i].data(), names[i].size())));
    JS::RootedValue value(cx);
    if (!name) return false;
    value.setString(name);
    JS::Rooted<JS::PropertyDescriptor> descriptor(cx);
    descriptor.set(JS::PropertyDescriptor::Data(value,
        {JS::PropertyAttribute::Configurable, JS::PropertyAttribute::Enumerable}));
    JS::ObjectOpResult result;
    JS::RootedId id(cx, JS::PropertyKey::Int(int32_t(i)));
    if (!JS_DefinePropertyById(cx, indexedTarget, id, descriptor, result) || !result.ok())
      return false;
  }
  return true;
}
bool RefreshIndexedDeclaration(JSContext* cx, JS::HandleObject view) {
  std::string block;
  if (!ReadViewBlock(cx, view, &block)) return false;
  const auto rows = ruyijs::domain::styling::CssDeclarations(block);
  std::vector<std::string> names;
  names.reserve(rows.size());
  for (const auto& row : rows) names.push_back(row.name);
  return RefreshIndexedDeclarationNames(cx, view, names);
}
}  // namespace ruyijs::web_styling::detail

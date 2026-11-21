#include "domain/styling/stylesheet.h"
#include "domain/styling/css_color_value.h"
#include "domain/styling/css_declarations.h"
#include "web_styling/internal.h"
#include "web_styling/adopted.h"
#include "web_styling/selector_input.h"
#include "web_styling/computed_source.h"
#include "web_styling/realm.h"
#include "js/Wrapper.h"
#include <algorithm>
#include <iterator>
#include <map>
#include <set>

namespace ruyijs::web_styling {
struct ComputedStyleReader::Impl {
  struct Scope {
    std::shared_ptr<const std::vector<ruyijs::domain::styling::StyleSheetAggregate>> aggregates;
    ruyijs::domain::styling::StyleResolutionContext computation;
  };
  State& state;
  JS::RootedObjectVector scopes;
  std::vector<std::unique_ptr<Scope>> sources;
  // Relation frames read once per session, shared by every element it reads.
  std::unique_ptr<SelectorInput> input;
  SelectorDependencies dependencies;
  explicit Impl(State& owner):state(owner),scopes(owner.cx) {}
};
namespace {
namespace css = ruyijs::domain::styling;
// Colour-valued properties serialize their computed colour as Firefox does.
bool ColorProperty(const std::string& name) {
  static const char suffix[] = "color";
  if (name.size() < 5 || name.compare(name.size() - 5, 5, suffix) != 0) return false;
  return name != "color-scheme" && name.rfind("color-", 0) != 0 && name != "print-color-adjust";
}
// The serialized custom-property suffix of a computed style. The map is shared
// (copy on write) by every element that inherits it, so the text is memoized by
// the map's identity and reused across elements of one getComputedStyle sweep.
std::string SerializeCustoms(const css::StyleDeclaration& computed) {
  const void* identity = computed.customs_identity();
  if (!identity) return {};
  static thread_local const void* cachedIdentity = nullptr;
  static thread_local std::string cachedText;
  if (identity != cachedIdentity) {
    std::string text;
    for (const auto& entry : computed.customs()) text += entry.first + ": " + entry.second.value + ";";
    cachedIdentity = identity;
    cachedText = std::move(text);
  }
  return cachedText;
}
// PRO exposes the 85-entry legacy computed declaration shape. Firefox's
// getComputedStyle shape is a separate surface and is not the reference here.
const char* const kLegacyComputedProperties[] = {
    "align-content", "align-items", "align-self", "background-color", "background-image", "border-bottom-color", "border-bottom-style", "border-bottom-width",
    "border-left-color", "border-left-style", "border-left-width", "border-right-color", "border-right-style", "border-right-width", "border-spacing", "border-top-color",
    "border-top-style", "border-top-width", "bottom", "box-sizing", "clear", "color", "column-gap", "content-visibility",
    "cursor", "display", "flex-basis", "flex-direction", "flex-grow", "flex-shrink", "flex-wrap", "float",
    "font-family", "font-feature-settings", "font-kerning", "font-language-override", "font-optical-sizing", "font-size", "font-size-adjust", "font-stretch",
    "font-style", "font-variant-alternates", "font-variant-caps", "font-variant-east-asian", "font-variant-emoji", "font-variant-ligatures", "font-variant-numeric", "font-variant-position",
    "font-variation-settings", "font-weight", "height", "justify-content", "left", "letter-spacing", "line-height", "list-style-type",
    "margin-bottom", "margin-left", "margin-right", "margin-top", "max-height", "max-width", "min-height", "min-width",
    "opacity", "order", "overflow-x", "overflow-y", "padding-bottom", "padding-left", "padding-right", "padding-top",
    "position", "right", "row-gap", "text-align", "text-decoration", "top", "transform", "vertical-align",
    "visibility", "white-space", "width", "word-spacing", "z-index",
};

const char* const kLegacyComputedDefaults[] = {
    "normal", "normal", "auto", "rgba(0, 0, 0, 0)", "none", "rgb(0, 0, 0)", "none", "0px",
    "rgb(0, 0, 0)", "none", "0px", "rgb(0, 0, 0)", "none", "0px", "0px", "rgb(0, 0, 0)",
    "none", "0px", "auto", "content-box", "none", "rgb(0, 0, 0)", "normal", "visible",
    "auto", "block", "auto", "row", "0", "1", "nowrap", "none",
    "sans-serif", "normal", "auto", "normal", "auto", "16px", "none", "100%",
    "normal", "normal", "normal", "normal", "normal", "normal", "normal", "normal",
    "normal", "400", "auto", "normal", "auto", "normal", "normal", "disc",
    "0px", "0px", "0px", "0px", "none", "none", "auto", "auto",
    "1", "0", "visible", "visible", "0px", "0px", "0px", "0px",
    "static", "auto", "normal", "start", "none", "auto", "none", "baseline",
    "visible", "normal", "auto", "0px", "auto",
};
static_assert(std::size(kLegacyComputedProperties) == std::size(kLegacyComputedDefaults));

void ProjectLegacyComputed(ComputedStyle* style) {
  // PRO's computed block is the sorted cascade-winner declarations followed by
  // the remaining legacy initial values in name order. Cascade winners outside
  // the legacy table stay visible; non-legacy initial values are dropped.
  // BuildComputedBlock appends inherited custom properties.
  static const char* const sides[] = {"margin-top", "margin-right", "margin-bottom", "margin-left"};
  std::map<std::string, std::string> computed;
  for (const auto& entry : style->values) computed.emplace(entry.first, entry.second);
  auto fix = [&](const std::string& name, std::string value) -> std::string {
    if (style->tag == "body")
      for (unsigned i = 0; i < 4; ++i)
        if (name == sides[i] && !(style->specifiedMarginMask & (1u << i))) return "8px";
    if (name == "text-decoration" && value == "none solid rgb(0, 0, 0)") return "none";
    return value;
  };
  std::map<std::string, std::string> declared;
  const size_t declaredCount = std::min(style->declaredCount, style->values.size());
  for (size_t i = 0; i < declaredCount; ++i) {
    const auto& entry = style->values[i];
    if (entry.first == "-x-lang") continue;
    if (entry.first.size() > 2 && entry.first.compare(0, 2, "--") == 0) continue;
    declared.emplace(entry.first, fix(entry.first, entry.second));
  }
  if (style->tag == "body")
    for (unsigned i = 0; i < 4; ++i)
      if (!(style->specifiedMarginMask & (1u << i))) declared[sides[i]] = "8px";
  std::vector<std::pair<std::string, std::string>> values;
  values.reserve(std::size(kLegacyComputedProperties) + declared.size() + 1);
  std::set<std::string> emitted;
  for (const auto& entry : declared) {
    values.emplace_back(entry);
    emitted.insert(entry.first);
  }
  for (size_t i = 0; i < std::size(kLegacyComputedProperties); ++i) {
    const std::string name = kLegacyComputedProperties[i];
    if (emitted.count(name)) continue;
    auto row = computed.find(name);
    std::string value = row != computed.end() ? row->second : kLegacyComputedDefaults[i];
    values.emplace_back(name, fix(name, std::move(value)));
  }
  // -x-lang remains available to the canvas language port but is not CSS.
  auto lang = computed.find("-x-lang");
  if (lang != computed.end()) values.emplace_back(lang->first, lang->second);
  style->values = std::move(values);
}

void ComputeColors(ComputedStyle* style) {
  std::string current;
  for (const auto& [name, value] : style->values)
    if (name == "color") current = ruyijs::domain::styling::CssComputedColor(value, {});
  for (auto& [name, value] : style->values) {
    if (!ColorProperty(name)) continue;
    std::string computed = ruyijs::domain::styling::CssComputedColor(value, current);
    if (!computed.empty()) value = std::move(computed);
  }
}
}  // namespace
ComputedStyleReader::ComputedStyleReader(State& state):impl_(std::make_unique<Impl>(state)) {}
ComputedStyleReader::~ComputedStyleReader() = default;
bool ComputedStyleReader::Read(JS::HandleObject element, ComputedStyle* out) {
  namespace css = ruyijs::domain::styling;
  State& state=impl_->state;
  if (!state.chainPort || !state.sheetsPort)
    return WebKit::Fail(state.cx, "getComputedStyle is not available");
  bool isElement = false;
  // The relation reader below supplies the cascade's ancestors on demand.
  // Validate here without copying an unused chain for every layout element.
  if (!state.chainPort(state.chainContext, element, nullptr, &isElement)) return false;
  if (!isElement) return WebKit::Fail(state.cx, "getComputedStyle requires an Element");
  if (!impl_->input) impl_->input = std::make_unique<SelectorInput>(state, element);
  else impl_->input->Target(element);
  SelectorInput& input = *impl_->input;
  const auto* node = input.Load();
  if (!node) return false;
  JS::RootedObject scope(state.cx);
  bool shadow=false;
  if (!state.cascadeScope(state.adoptedContext,element,&scope,&shadow)) return false;
  scope=js::CheckedUnwrapStatic(scope);
  size_t index=0;
  for(;index<impl_->scopes.length();++index) if(impl_->scopes[index]==scope) break;
  if(index==impl_->scopes.length()) {
    auto entry=std::make_unique<Impl::Scope>();
    std::shared_ptr<const SelectorDependencies> dependencies;
    if (!ReadCascadeSources(state,element,&entry->aggregates,&dependencies,&entry->computation.store)||
        !impl_->scopes.append(scope)) return false;
    if (dependencies) impl_->dependencies.Merge(*dependencies);
    impl_->sources.push_back(std::move(entry));
  }
  auto& source=*impl_->sources[index];
  std::vector<const css::StyleSheetAggregate*> pointers;
  pointers.reserve(source.aggregates->size());
  for (const auto& aggregate : *source.aggregates) pointers.push_back(&aggregate);
  const auto computed = css::StyleResolutionService::compute_style(pointers, *node, &source.computation);
  if (!input.ok()) return false;
  ComputedStyle result;
  result.fontSize = computed.computed_font_size;
  result.rootFontSize = computed.root_font_size;
  result.specifiedMarginMask = computed.specified_margin_mask;
  result.specifiedBorderMask = computed.specified_border_mask;
  result.specifiedPaddingMask = computed.specified_padding_mask;
  result.tag = node->tag;
  result.legacyProjected = true;
  result.declaredCount = computed.all().size();
  for (const auto& entry : computed.all())
    result.values.emplace_back(entry.first, entry.first == "line-height"
        ? css::CssResolvedLineHeight(computed) : entry.second.value);
  for (const auto& entry : css::StyleResolutionService::initial_values())
    if (!computed.all().count(entry.first)) result.values.emplace_back(entry.first, entry.second);
  result.customsText = SerializeCustoms(computed);
  result.childInputs = css::StyleResolutionService::child_inputs(computed);
  result.inheritsReset = computed.inherits_reset;
  ComputeColors(&result);
  ProjectLegacyComputed(&result);
  *out = std::move(result);
  return true;
}
bool ComputedStyleReader::ReadDefault(JS::HandleObject element, ComputedStyle* out) {
  namespace css = ruyijs::domain::styling;
  State& state=impl_->state;
  if (!state.chainPort) return WebKit::Fail(state.cx, "getDefaultComputedStyle is not available");
  bool isElement = false;
  if (!state.chainPort(state.chainContext, element, nullptr, &isElement)) return false;
  if (!isElement) return WebKit::Fail(state.cx, "getDefaultComputedStyle requires an Element");
  if (!impl_->input) impl_->input = std::make_unique<SelectorInput>(state, element);
  else impl_->input->Target(element);
  const auto* node = impl_->input->Load();
  if (!node) return false;
  // A detached copy of the ancestor chain without inline declarations; keys are
  // cleared so the shared cascade cache never mixes it with author results.
  std::vector<std::unique_ptr<css::ElementNode>> chain;
  for (const css::ElementNode* cursor = node; cursor && chain.size() < 1024; cursor = cursor->related('>')) {
    auto copy = std::make_unique<css::ElementNode>(*cursor);
    copy->inline_style.clear();
    copy->key = 0;
    copy->parent = nullptr;
    copy->relations = nullptr;  // the copy is self-contained: parents only
    chain.push_back(std::move(copy));
    if (!impl_->input->ok()) return false;
  }
  for (size_t i = 0; i + 1 < chain.size(); ++i) chain[i]->parent = chain[i + 1].get();
  if (!impl_->input->ok()) return false;
  const std::vector<const css::StyleSheetAggregate*> none;
  const auto computed = css::StyleResolutionService::compute_style(none, *chain.front());
  ComputedStyle result;
  result.fontSize = computed.computed_font_size;
  result.rootFontSize = computed.root_font_size;
  result.tag = chain.front()->tag;
  result.legacyProjected = true;
  result.declaredCount = computed.all().size();
  for (const auto& entry : computed.all())
    result.values.emplace_back(entry.first, entry.first == "line-height"
        ? css::CssResolvedLineHeight(computed) : entry.second.value);
  for (const auto& entry : css::StyleResolutionService::initial_values())
    if (!computed.all().count(entry.first)) result.values.emplace_back(entry.first, entry.second);
  result.customsText = SerializeCustoms(computed);
  ComputeColors(&result);
  ProjectLegacyComputed(&result);
  *out = std::move(result);
  return true;
}
bool ReadComputed(State& state, JS::HandleObject element, ComputedStyle* out) {
  ComputedStyleReader reader(state);
  return reader.Read(element,out);
}
bool Realm::ReadComputed(JS::HandleObject element, ComputedStyle* out) {
  return web_styling::ReadComputed(*state_, element, out);
}
size_t ComputedStyleReader::Scopes() const { return impl_->scopes.length(); }
const SelectorDependencies& ComputedStyleReader::Dependencies() const { return impl_->dependencies; }
bool Realm::CascadeStamp(JS::HandleObject element, uint64_t* stamp) {
  return web_styling::CascadeStamp(*state_, element, stamp);
}
std::unique_ptr<ComputedStyleReader> Realm::BeginComputedRead() {
  return std::make_unique<ComputedStyleReader>(*state_);
}
}

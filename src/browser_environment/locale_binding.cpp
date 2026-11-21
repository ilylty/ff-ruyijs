// The Intl default locale of a realm (xpc_LocalizeRuntime's counterpart in this
// runtime). Firefox derives it from the OS regional-preferences locale, so a
// page whose navigator.language is a different language than the operating
// system would otherwise read one locale from navigator.language and another
// from Intl.DateTimeFormat().resolvedOptions().
//
// The rule reconciles the two: the primary language subtags decide. When they
// agree the OS locale wins, because it carries the region (en-US vs en-GB,
// zh-Hans-CN vs zh-Hant-TW); when they differ navigator.language wins, so the
// language a page reads is the language Intl formats with. An unset
// locale.osLocale means the host's own OS locale and is compared the same way:
// on a Chinese host navigator.language=zh-Hant-TW keeps the host locale while
// navigator.language=de-DE takes effect.
#include "browser_environment/state.h"
#include "js/LocaleSensitive.h"
#include <string>

namespace ruyijs::browser_environment {
namespace {
// The primary language subtag: everything before the first '-' or '_', ASCII
// lower-cased. Both separators are cut because a profile may spell the tag with
// either (navigator.language is copied verbatim from the profile).
std::string PrimarySubtag(const std::string& tag) {
  std::string result;
  for (const char c : tag) {
    if (c == '-' || c == '_') break;
    result.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
  }
  return result;
}
// The runtime's own default locale, which is the host OS locale: Intl uses it
// whenever a realm carries no override. JS_GetDefaultLocale canonicalizes it
// through ICU, so the string is a well-formed BCP 47 tag.
std::string HostLocale(JSContext* cx) {
  JS::UniqueChars locale = JS_GetDefaultLocale(cx);
  return locale ? std::string(locale.get()) : std::string();
}
}  // namespace
std::string ReconcileLocale(JSContext* cx, const std::string& osLocale,
                            const std::string& navigatorLanguage) {
  const std::string host = osLocale.empty() ? HostLocale(cx) : osLocale;
  if (host.empty()) return navigatorLanguage;
  if (navigatorLanguage.empty()) return host;
  return PrimarySubtag(host) == PrimarySubtag(navigatorLanguage) ? host
                                                                : navigatorLanguage;
}
}  // namespace ruyijs::browser_environment

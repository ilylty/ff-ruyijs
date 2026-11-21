// Names the DOM member a function object stands for.
//
// ruyijs installs DOM members from two independent layers: the DocumentModel
// (State::Method / State::Accessor) and the WebKit bridges (PageHelpers::Member /
// Accessor / Constructor, WebKit::Method / Accessor). Both funnel through the
// registry below, so one lookup recovers the interface name for any callee.
//
// The registry is filled only during installation, when nobody is calling, and
// read on every DOM call. Keys are the function objects themselves, which are
// permanent for the realm's lifetime.
#ifndef RUYIJS_MEMBER_REGISTRY_H
#define RUYIJS_MEMBER_REGISTRY_H

#include "js/TypeDecls.h"

namespace ruyijs {

// Records that |member| implements |iface|.<name>. |kind| is one of
// "get" / "set" / "call" / "construct".
void RegisterMemberName(const JSObject* member, const char* iface, const char* name,
                        const char* kind);

// Recovers a registered name. Returns false for any function that is not a DOM
// member, which is the common case for arbitrary page code.
bool LookupMemberName(const JSObject* member, const char** iface, const char** name,
                      const char** kind);

}  // namespace ruyijs

#endif

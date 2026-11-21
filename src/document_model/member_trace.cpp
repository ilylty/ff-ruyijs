#include "document_model/state.h"

#include "runtime/member_registry.h"

namespace ruyijs::document_model {

// The DocumentModel and the WebKit bridges install members in different places,
// but a callee has to be recognizable from either side, so both land in the one
// registry in runtime/member_registry.h rather than in a table per layer.
void RecordMemberBinding(JSObject* member, const char* iface, const char* name,
                         const char* kind) {
  RegisterMemberName(member, iface, name, kind);
}

bool LookupMemberBinding(const JSObject* member, const char** iface,
                         const char** name, const char** kind) {
  return LookupMemberName(member, iface, name, kind);
}

}  // namespace ruyijs::document_model

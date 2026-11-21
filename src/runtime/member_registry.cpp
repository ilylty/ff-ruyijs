#include "runtime/member_registry.h"

#include <string>
#include <unordered_map>

namespace ruyijs {
namespace {

struct Entry {
  std::string iface;
  std::string name;
  std::string kind;
};

std::unordered_map<const JSObject*, Entry>& Table() {
  static std::unordered_map<const JSObject*, Entry> table;
  return table;
}

}  // namespace

void RegisterMemberName(const JSObject* member, const char* iface, const char* name,
                        const char* kind) {
  if (!member) {
    return;
  }
  Table()[member] = Entry{iface ? iface : "", name ? name : "",
                          kind ? kind : "call"};
}

bool LookupMemberName(const JSObject* member, const char** iface, const char** name,
                      const char** kind) {
  if (!member) {
    return false;
  }
  const auto found = Table().find(member);
  if (found == Table().end()) {
    return false;
  }
  *iface = found->second.iface.c_str();
  *name = found->second.name.c_str();
  *kind = found->second.kind.c_str();
  return true;
}

}  // namespace ruyijs

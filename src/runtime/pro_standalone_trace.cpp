#include "runtime/pro_standalone_trace.h"

namespace ruyijs {

ProStandaloneTraceGuard::ProStandaloneTraceGuard() { RuyijsProStart(); }
ProStandaloneTraceGuard::~ProStandaloneTraceGuard() { RuyijsProStop(); }

namespace pro_trace {

bool Configure(int argc, char** argv) { return RuyijsProConfigure(argc, argv); }

bool Configure(const std::vector<std::string>& arguments) {
  std::vector<char*> pointers;
  std::vector<std::string> storage = arguments;
  storage.insert(storage.begin(), "js.exe");
  for (std::string& text : storage) pointers.push_back(text.data());
  pointers.push_back(nullptr);
  return RuyijsProConfigure(static_cast<int>(storage.size()), pointers.data());
}

void Start() { RuyijsProStart(); }
void Stop() { RuyijsProStop(); }
void EmitPageEvents(const std::string& url) { RuyijsProEmitPageEvents(url.c_str()); }

void Registry(const char* kind, const char* iface, const char* member,
              const char* valueText, const char* valueObjText,
              const char* returnText, const char* returnObjText,
              const char* thisText, const char* argsText,
              const char* postArgsText) {
  RuyijsProRegistry(kind, iface, member, valueText, valueObjText, returnText,
                    returnObjText, thisText, argsText, postArgsText);
}

}

}

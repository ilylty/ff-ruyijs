#ifndef RUYIJS_PRO_STANDALONE_TRACE_H
#define RUYIJS_PRO_STANDALONE_TRACE_H

#include <string>
#include <vector>

// Pro standalone trace lives in the engine's vm/StandaloneTrace.cpp so its
// records come from the same call/eval hooks as the runtime. This header is the
// host-side adapter; the exported C entry points are linked from js_static.lib.
extern "C" {
bool RuyijsProConfigure(int argc, char** argv);
void RuyijsProStart();
void RuyijsProStop();
void RuyijsProEmitPageEvents(const char* url);
void RuyijsProRegistry(const char* kind, const char* iface, const char* member,
                       const char* valueText, const char* valueObjText,
                       const char* returnText, const char* returnObjText,
                       const char* thisText, const char* argsText,
                       const char* postArgsText);
}

namespace ruyijs {

class ProStandaloneTraceGuard final {
 public:
  ProStandaloneTraceGuard();
  ~ProStandaloneTraceGuard();
};

namespace pro_trace {
bool Configure(int argc, char** argv);
bool Configure(const std::vector<std::string>& arguments);
void Start();
void Stop();
void EmitPageEvents(const std::string& url);
void Registry(const char* kind, const char* iface, const char* member,
              const char* valueText, const char* valueObjText,
              const char* returnText, const char* returnObjText,
              const char* thisText, const char* argsText,
              const char* postArgsText);
}

}

#endif

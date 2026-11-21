#ifndef RUYIJS_CF_ACCESS_TRACE_H
#define RUYIJS_CF_ACCESS_TRACE_H

#include "js/CallArgs.h"
#include "js/Value.h"
#include <string>

// Deferred domtrace record. The WebIDL funnel calls Observe() at native entry,
// where args.rval() is not yet set. This scope keeps the JS::CallArgs alive and
// emits the get/set/call record at native exit, with the real values.
namespace ruyijs {

class CfAccessTrace final {
 public:
  CfAccessTrace(JSContext* cx, const JS::CallArgs& args,
                const char* iface, const char* member, int type,
                bool enabled);
  ~CfAccessTrace();
  CfAccessTrace(const CfAccessTrace&) = delete;
  CfAccessTrace& operator=(const CfAccessTrace&) = delete;
  bool Finish(bool ok) { ok_ = ok; return ok; }
 private:
  JSContext* cx_;
  JS::CallArgs args_;
  std::string iface_;
  std::string member_;
  int type_;
  bool enabled_;
  // Success by default: most WebIDL natives have many early returns, and a
  // scope that never calls Finish() still observed a completed access.
  bool ok_ = true;
};

}  // namespace ruyijs

#endif

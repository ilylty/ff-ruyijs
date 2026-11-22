#ifndef RUYIJS_WASM_STREAM_H
#define RUYIJS_WASM_STREAM_H

#include "js/TypeDecls.h"

namespace ruyijs {
namespace wasm_stream {

// Install the native Response-aware JS::ConsumeStreamCallback on |cx|.
// This keeps WebAssembly.instantiateStreaming as SpiderMonkey's own function:
// the host callback validates the Response and feeds its bytes to the
// streaming compiler; no JavaScript wrapper is installed on the page.
void InitForContext(JSContext* cx);

}  // namespace wasm_stream
}  // namespace ruyijs

#endif

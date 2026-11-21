// HTTP packet tracing for challenge research (RUYIJS_DOM_HTTP_PACKET_TRACE).
//
// ruyitrace records every Necko exchange at the C++ layer
// (MOZ_DOM_HTTP_PACKET_TRACE), including full request and response headers,
// bodies, per-phase timings, the HTTP version and the initiator stack. ruyijs
// performs its HTTP in the same process, so the equivalent record can be
// produced from NativeHttpRequest and HttpResult without touching the transport.
//
// The record shape follows ruyitrace's http_packet.v1 document so the two can be
// diffed section by section:
//   {schema, url, method, status, requestHeaders[{name,value}],
//    responseHeaders[{name,value}], params{...}, responseBody{...}, meta{...}}
//
// Off by default; RUYIJS_DOM_HTTP_PACKET_TRACE names the output file and
// RUYIJS_DOM_HTTP_PACKET_LIMIT bounds the record count (0 = unlimited).
//
// Bodies over RUYIJS_DOM_HTTP_PACKET_BODY_BYTES (default 65536) are recorded
// truncated, with the full length still reported, because challenge payloads
// reach hundreds of kilobytes.
#ifndef RUYIJS_RUNTIME_HTTP_TRACE_H
#define RUYIJS_RUNTIME_HTTP_TRACE_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ruyijs::http_trace {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Allocates the identifier for a request whose response will be traced later.
// Returns 0 when tracing is off.
uint64_t BeginRequest(const std::string& method, const std::string& url,
                      const HeaderList& headers, const std::string& body,
                      const char* requestKind = "document");

// Paired with BeginRequest. Writes the whole exchange: ruyitrace stores one
// document per request, so the request fields are re-emitted with the response.
void EndRequest(uint64_t id, int status, const std::string& statusText,
                const HeaderList& headers, const std::string& body,
                const std::string& finalURL, const std::string& protocol,
                const std::string& error);

bool Enabled();

}  // namespace ruyijs::http_trace

#endif  // RUYIJS_RUNTIME_HTTP_TRACE_H

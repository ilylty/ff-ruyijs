// HTTP packet tracing — implementation. See http_trace.h.
//
// ruyitrace writes one document per exchange, with named sections for headers,
// body, params and metadata. ruyijs records the same sections from the values it
// already holds at the transport boundary. Sections ruyijs cannot observe (the
// initiator stack, cache provenance, per-phase offsets) are emitted with their
// ruyitrace key present and neutral values, so a field-by-field diff shows a real
// difference instead of a missing key.

#include "runtime/http_trace.h"
#include "vm/StandaloneTrace.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <strings.h>
#include <unistd.h>
#endif

namespace ruyijs::http_trace {
namespace {

FILE* Handle() {
  static FILE* file = []() -> FILE* {
    const char* path = std::getenv("RUYIJS_DOM_HTTP_PACKET_TRACE");
    if (!path || !*path) {
      return nullptr;
    }
    return std::fopen(path, "ab");
  }();
  return file;
}

// ruyitrace keeps one http_packet document per request and an index.jsonl that
// lets a reader find them. ruyijs keeps every document in one log file, so the
// index points at that file instead of a per-request filename.
FILE* IndexHandle() {
  static FILE* file = []() -> FILE* {
    const char* path = std::getenv("RUYIJS_DOM_HTTP_PACKET_INDEX");
    if (path && *path) {
      return std::fopen(path, "ab");
    }
    const char* log = std::getenv("RUYIJS_DOM_HTTP_PACKET_TRACE");
    if (!log || !*log) {
      return nullptr;
    }
    std::string derived(log);
    const size_t slash = derived.find_last_of("/\\");
    if (slash == std::string::npos) {
      derived.assign("index.jsonl");
    } else {
      derived.resize(slash + 1);
      derived.append("index.jsonl");
    }
    return std::fopen(derived.c_str(), "ab");
  }();
  return file;
}

uint64_t Limit() {
  static uint64_t limit = []() -> uint64_t {
    const char* text = std::getenv("RUYIJS_DOM_HTTP_PACKET_LIMIT");
    if (!text || !*text) {
      return 0;
    }
    return static_cast<uint64_t>(std::strtoull(text, nullptr, 10));
  }();
  return limit;
}

size_t BodyBudget() {
  static size_t budget = []() -> size_t {
    const char* text = std::getenv("RUYIJS_DOM_HTTP_PACKET_BODY_BYTES");
    if (!text || !*text) {
      return 65536;
    }
    return static_cast<size_t>(std::strtoull(text, nullptr, 10));
  }();
  return budget;
}

uint64_t NextSeq() {
  static uint64_t seq = 0;
  return seq++;
}

long long CurrentPid() {
#if defined(_WIN32)
  return static_cast<long long>(GetCurrentProcessId());
#else
  return static_cast<long long>(getpid());
#endif
}

int64_t NowMicros() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

// L4.1: packet index timestamps are absolute epoch ms, matching ruyitrace.
// NowMicros() stays for internal durations and the legacy packet.log stream.
int64_t NowEpochMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void AppendJsonString(std::string* out, const char* data, size_t length) {
  out->push_back('"');
  // Bytes at or above 0x80 are only passed through when they form valid UTF-8.
  // Response bodies here can be binary (images, compressed payloads), and a raw
  // high byte would make the whole record invalid JSON for any strict parser.
  size_t i = 0;
  while (i < length) {
    const unsigned char c = static_cast<unsigned char>(data[i]);
    switch (c) {
      case '"': out->append("\\\""); ++i; continue;
      case '\\': out->append("\\\\"); ++i; continue;
      case '\n': out->append("\\n"); ++i; continue;
      case '\r': out->append("\\r"); ++i; continue;
      case '\t': out->append("\\t"); ++i; continue;
      default: break;
    }
    if (c < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
      out->append(buffer);
      ++i;
      continue;
    }
    if (c < 0x80) {
      out->push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    // Multi-byte UTF-8 sequence: validate the shape before copying it through.
    size_t need = 0;
    if ((c & 0xE0) == 0xC0) need = 1;
    else if ((c & 0xF0) == 0xE0) need = 2;
    else if ((c & 0xF8) == 0xF0) need = 3;
    bool valid = need > 0 && i + need < length;
    if (valid) {
      for (size_t k = 1; k <= need; ++k) {
        if (i + k >= length || (static_cast<unsigned char>(data[i + k]) & 0xC0) != 0x80) {
          valid = false;
          break;
        }
      }
    }
    if (valid) {
      out->append(data + i, need + 1);
      i += need + 1;
    } else {
      // Not valid UTF-8: emit the byte as a Latin-1 code point so the byte's
      // identity survives and the record stays parseable.
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
      out->append(buffer);
      ++i;
    }
  }
  out->push_back('"');
}

void AppendJsonString(std::string* out, const std::string& text) {
  AppendJsonString(out, text.data(), text.size());
}

void AppendNumber(std::string* out, const char* format, long long value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), format, value);
  out->append(buffer);
}

// SHA-256 for the packet index. ruyitrace hashes the raw request and response
// bodies; ruyijs keeps both in memory at EndRequest, so the digest can be
// computed without another transport read.
std::string Sha256Hex(const std::string& input) {
  static const uint32_t k[] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  const uint64_t bitLength = static_cast<uint64_t>(input.size()) * 8u;
  std::vector<uint8_t> data(input.begin(), input.end());
  data.push_back(0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8) {
    data.push_back(static_cast<uint8_t>(bitLength >> shift));
  }
  auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
  for (size_t offset = 0; offset < data.size(); offset += 64) {
    uint32_t w[64];
    for (size_t i = 0; i < 16; ++i) {
      const size_t at = offset + i * 4;
      w[i] = (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) |
             (uint32_t(data[at + 2]) << 8) | uint32_t(data[at + 3]);
    }
    for (size_t i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (size_t i = 0; i < 64; ++i) {
      const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      hh = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  std::string hex;
  hex.reserve(64);
  char buffer[9];
  for (uint32_t value : h) {
    std::snprintf(buffer, sizeof(buffer), "%08x", value);
    hex.append(buffer);
  }
  return hex;
}

// ruyitrace stores headers as [{name, value}], preserving duplicates and order,
// which matters for cookie and cf-chl headers.
void AppendHeaders(std::string* out, const HeaderList& headers) {
  out->push_back('[');
  for (size_t i = 0; i < headers.size(); ++i) {
    if (i) {
      out->push_back(',');
    }
    out->append("{\"name\":");
    AppendJsonString(out, headers[i].first);
    out->append(",\"value\":");
    AppendJsonString(out, headers[i].second);
    out->push_back('}');
  }
  out->push_back(']');
}

const char* HeaderValue(const HeaderList& headers, const char* name) {
  const size_t length = std::strlen(name);
  for (const auto& entry : headers) {
    if (entry.first.size() != length) {
      continue;
    }
#if defined(_WIN32)
    const int differs = _strnicmp(entry.first.c_str(), name, length);
#else
    const int differs = strncasecmp(entry.first.c_str(), name, length);
#endif
    if (differs == 0) {
      return entry.second.c_str();
    }
  }
  return "";
}

void AppendBodyBlock(std::string* out, const std::string& body,
                     const char* contentType, bool encoded) {
  const size_t budget = BodyBudget();
  const size_t kept = body.size() > budget ? budget : body.size();
  out->append("{\"kind\":");
  AppendJsonString(out, contentType && *contentType ? contentType : "text");
  out->append(",\"contentType\":");
  AppendJsonString(out, contentType ? contentType : "");
  out->append(",\"charset\":\"utf-8\",\"text\":");
  AppendJsonString(out, body.data(), kept);
  out->append(",\"bytes\":");
  AppendNumber(out, "%lld", static_cast<long long>(body.size()));
  out->append(",\"transferredSize\":");
  AppendNumber(out, "%lld", static_cast<long long>(body.size()));
  out->append(",\"truncated\":");
  out->append(body.size() > budget ? "true" : "false");
  out->append(",\"isContentEncoded\":");
  out->append(encoded ? "true" : "false");
  // ruyitrace carries a base64 rendering and a sha256 of the body. The digest
  // is computed here from the untruncated body; base64 stays empty because no
  // consumer in the diff uses it.
  out->append(",\"base64\":\"\",\"sha256\":");
  AppendJsonString(out, Sha256Hex(body));
  out->append(",\"compressionEncodings\":[]}");
}

void Emit(const std::string& record) {
  FILE* file = Handle();
  if (!file) {
    return;
  }
  std::fwrite(record.data(), 1, record.size(), file);
  std::fflush(file);
}

// Pending request data, so the exit record can emit the whole exchange the way
// ruyitrace does (one document per request, not two halves).
struct Pending {
  std::string method;
  std::string url;
  HeaderList headers;
  std::string body;
  std::string requestKind;
  int64_t startMicros = 0;
  int64_t startEpochMs = 0;
};
std::vector<Pending>& PendingList() {
  static std::vector<Pending> list;
  return list;
}

}  // namespace

bool Enabled() { return Handle() != nullptr; }

uint64_t BeginRequest(const std::string& method, const std::string& url,
                      const HeaderList& headers, const std::string& body,
                      const char* requestKind) {
  if (!Enabled()) {
    return 0;
  }
  const uint64_t limit = Limit();
  const uint64_t seq = NextSeq();
  if (limit > 0 && seq >= limit) {
    return 0;
  }
  Pending pending;
  pending.method = method;
  pending.url = url;
  pending.headers = headers;
  pending.body = body;
  pending.requestKind = requestKind && *requestKind ? requestKind : "document";
  pending.startMicros = NowMicros();
  pending.startEpochMs = NowEpochMillis();
  PendingList().push_back(std::move(pending));
  // The id is the index plus one so it is never 0, which BeginRequest uses to
  // mean "not traced".
  return seq + 1;
}

void EndRequest(uint64_t id, int status, const std::string& statusText,
                const HeaderList& headers, const std::string& body,
                const std::string& finalURL, const std::string& protocol,
                const std::string& error) {
  if (!Enabled() || id == 0) {
    return;
  }
  const uint64_t index = id - 1;
  Pending* pending = nullptr;
  if (index < PendingList().size()) {
    pending = &PendingList()[index];
  }

  const int64_t endMicros = NowMicros();
  const int64_t endEpochMs = NowEpochMillis();
  const int64_t startEpochMs = pending ? pending->startEpochMs : endEpochMs;
  const std::string url = pending ? pending->url : finalURL;
  const std::string method = pending ? pending->method : "";
  const std::string requestKind = pending ? pending->requestKind : std::string("document");
  const int64_t startMicros = pending ? pending->startMicros : endMicros;

  const char* requestType =
      pending ? HeaderValue(pending->headers, "Content-Type") : "";
  const char* responseType = HeaderValue(headers, "content-type");

  std::string record;
  record.reserve(2048 + body.size());
  record.append("{\"schema\":\"http_packet.v1\",\"seq\":");
  AppendNumber(&record, "%lld", static_cast<long long>(index));
  record.append(",\"pid\":");
  AppendNumber(&record, "%lld", CurrentPid());
  record.append(",\"url\":");
  AppendJsonString(&record, url);
  record.append(",\"method\":");
  AppendJsonString(&record, method);
  record.append(",\"status\":");
  AppendNumber(&record, "%d", status);
  record.append(",\"requestHeaders\":");
  AppendHeaders(&record, pending ? pending->headers : HeaderList{});
  record.append(",\"responseHeaders\":");
  AppendHeaders(&record, headers);

  // params mirrors ruyitrace's request-body section.
  record.append(",\"params\":{\"query\":[],\"postText\":");
  AppendJsonString(&record, pending ? pending->body : std::string());
  record.append(",\"postJson\":null,\"postForm\":[],\"postBytes\":");
  AppendNumber(&record, "%lld",
               static_cast<long long>(pending ? pending->body.size() : 0));
  record.append(",\"postSha256\":");
  AppendJsonString(&record, Sha256Hex(pending ? pending->body : std::string()));
  record.append("}");

  record.append(",\"responseBody\":");
  if (!error.empty()) {
    AppendJsonString(&record, error);
  } else {
    AppendBodyBlock(&record, body, responseType, false);
  }

  record.append(",\"meta\":{\"schema\":\"http_packet.v1\",\"requestId\":");
  char requestId[48];
  std::snprintf(requestId, sizeof(requestId), "%lld-%06llu", CurrentPid(),
                static_cast<unsigned long long>(index));
  AppendJsonString(&record, requestId);
  record.append(",\"channelId\":0,\"seq\":");
  AppendNumber(&record, "%lld", static_cast<long long>(index));
  record.append(",\"pid\":");
  AppendNumber(&record, "%lld", CurrentPid());
  record.append(",\"processType\":\"parent\",\"requestKind\":");
  AppendJsonString(&record, requestKind);
  record.append(",\"statusText\":");
  AppendJsonString(&record, statusText);
  record.append(",\"httpVersion\":");
  AppendJsonString(&record, protocol);
  record.append(",\"finalURL\":");
  AppendJsonString(&record, finalURL.empty() ? url : finalURL);
  record.append(",\"browsingContextId\":0,\"innerWindowId\":0");
  record.append(",\"contentPolicyType\":\"\",\"documentUrl\":\"\",\"topLevelUrl\":\"\"");
  record.append(",\"initiator\":\"\",\"extension\":\"\",\"securityState\":\"\"");
  record.append(",\"blockedReason\":\"\",\"fromCache\":false,\"fromServiceWorker\":false");
  record.append(",\"startMs\":");
  AppendNumber(&record, "%lld", static_cast<long long>(startMicros / 1000));
  record.append(",\"endMs\":");
  AppendNumber(&record, "%lld", static_cast<long long>(endMicros / 1000));
  record.append(",\"durationMs\":");
  AppendNumber(&record, "%lld", static_cast<long long>((endMicros - startMicros) / 1000));
  // Per-phase offsets are not exposed by the ruyijs transport; the keys stay so a
  // diff distinguishes "no value" from "no field".
  record.append(",\"offsets\":{\"domainLookupStart\":0,\"domainLookupEnd\":0,\"connectStart\":0,");
  record.append("\"connectEnd\":0,\"secureConnectionStart\":0,\"requestStart\":");
  AppendNumber(&record, "%lld", static_cast<long long>(startMicros / 1000));
  record.append(",\"responseStart\":");
  AppendNumber(&record, "%lld", static_cast<long long>(endMicros / 1000));
  record.append(",\"responseEnd\":");
  AppendNumber(&record, "%lld", static_cast<long long>(endMicros / 1000));
  record.append("},\"timings\":{}}");

  record.append(",\"stack\":{\"schema\":\"http_packet.stack.v1\",\"channelId\":0,");
  record.append("\"source\":\"\",\"pid\":");
  AppendNumber(&record, "%lld", CurrentPid());
  record.append(",\"processType\":\"parent\",\"stacktraceAvailable\":false,");
  record.append("\"stacktrace\":[],\"lastFrame\":null}}\n");
  Emit(record);

  // Index companion: one line per exchange, matching ruyitrace's
  // http_packet.index.v1 field set so the two streams can be diffed.
  if (FILE* indexFile = IndexHandle()) {
    char requestId[48];
    std::snprintf(requestId, sizeof(requestId), "%lld-%06llu", CurrentPid(),
                  static_cast<unsigned long long>(index));
    const char* contentType = HeaderValue(headers, "content-type");
    std::string entry;
    entry.reserve(512);
    entry.append("{\"schema\":\"http_packet.index.v1\",\"requestId\":");
    AppendJsonString(&entry, requestId);
    entry.append(",\"seq\":");
    AppendNumber(&entry, "%lld", static_cast<long long>(index));
    entry.append(",\"pid\":");
    AppendNumber(&entry, "%lld", CurrentPid());
    entry.append(",\"processType\":\"parent\",\"file\":");
    const char* log = std::getenv("RUYIJS_DOM_HTTP_PACKET_TRACE");
    AppendJsonString(&entry, log && *log ? log : "");
    entry.append(",\"url\":");
    AppendJsonString(&entry, url);
    entry.append(",\"method\":");
    AppendJsonString(&entry, method);
    entry.append(",\"status\":");
    AppendNumber(&entry, "%d", status);
    entry.append(",\"requestKind\":");
    AppendJsonString(&entry, requestKind);
    entry.append(",\"contentType\":");
    AppendJsonString(&entry, contentType ? contentType : "");
    entry.append(",\"responseSha256\":");
    AppendJsonString(&entry, Sha256Hex(error.empty() ? body : error));
    entry.append(",\"requestSha256\":");
    AppendJsonString(&entry, Sha256Hex(pending ? pending->body : std::string()));
    // L4.1: response body digest under the contract's explicit field name.
    // `body_sha256_basis` states which bytes were hashed; no silent alias.
    entry.append(",\"body_sha256\":");
    AppendJsonString(&entry, Sha256Hex(error.empty() ? body : error));
    entry.append(",\"body_sha256_basis\":\"response_body_sha256\"");
    entry.append(",\"startMs\":");
    AppendNumber(&entry, "%lld", static_cast<long long>(startEpochMs));
    entry.append(",\"startMs_kind\":\"epoch_ms\"");
    entry.append(",\"endMs\":");
    AppendNumber(&entry, "%lld", static_cast<long long>(endEpochMs));
    entry.append(",\"endMs_kind\":\"epoch_ms\"");
    entry.append(",\"durationMs\":");
    AppendNumber(&entry, "%lld",
                 static_cast<long long>((endMicros - startMicros) / 1000));
    entry.append(",\"fromCache\":false,\"fromServiceWorker\":false}\n");
    std::fwrite(entry.data(), 1, entry.size(), indexFile);
    std::fflush(indexFile);
  }

  // Deferred full trace: arm the heavy opcode/jscall streams once the URL of
  // interest has actually been observed on the wire. Starting them at process
  // boot makes the page too slow to ever reach this packet.
  if (const char* trigger = std::getenv("RUYIJS_TRACE_HEAVY_URL")) {
    if (*trigger && url.find(trigger) != std::string::npos) {
      js::standalone_trace::SetHeavyTraceArmed(true);
    }
  }

  if (pending) {
    PendingList()[index] = Pending{};
  }
}

}  // namespace ruyijs::http_trace

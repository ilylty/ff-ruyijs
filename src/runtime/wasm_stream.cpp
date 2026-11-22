// Native WebAssembly streaming support for ruyijs.
//
// SpiderMonkey defines WebAssembly.instantiateStreaming/compileStreaming and
// passes the resolved Response object to JS::ConsumeStreamCallback. The shell's
// default callback only understands BufferSource and StreamCacheEntryObject,
// so a page's `instantiateStreaming(new Response(...))` rejects before any
// bytes reach the streaming compiler. This unit replaces that callback with
// one that understands the ruyijs Response object.
//
// Validation (status, Content-Type, body lock) follows Blink's
// v8_wasm_response_extensions.cc. No page-visible JavaScript is installed.
#include "runtime/wasm_stream.h"

#include "resource_network/body.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "js/ArrayBuffer.h"
#include "js/CallArgs.h"
#include "js/Exception.h"
#include "js/Promise.h"
#include "js/StreamConsumer.h"
#include "js/String.h"
#include "js/Vector.h"
#include "js/Value.h"

#include <cstring>
#include <string>
#include <thread>

namespace ruyijs {
namespace wasm_stream {
namespace {

constexpr uint32_t kConsumerSlot = 0;
constexpr size_t kErrorResponse = 1;
constexpr size_t kErrorRead = 2;

// SpiderMonkey turns streamError(code) into the wasm promise's rejection by
// calling this callback on the JS thread.
void ReportStreamError(JSContext* cx, size_t errorCode) {
  if (errorCode == kErrorResponse) {
    JS_ReportErrorASCII(cx, "WebAssembly streaming: invalid Response");
  } else {
    JS_ReportErrorASCII(cx, "WebAssembly streaming failed");
  }
}

bool RejectStream(JSContext* cx, JS::StreamConsumer*,
                  size_t, const char* message) {
  // A synchronous callback failure is reported on cx and returned as false;
  // SpiderMonkey rejects the pending wasm promise from that exception. Calling
  // streamError here would double-report and crash.
  JS_ReportErrorASCII(cx, "%s", message);
  return false;
}

// Resolve an object to an ArrayBuffer. TypedArray/DataView expose .buffer.
bool AsArrayBuffer(JSContext* cx, JS::HandleObject object,
                   JS::MutableHandleObject buffer) {
  if (JS::IsArrayBufferObject(object)) {
    buffer.set(object);
    return true;
  }
  JS::RootedValue backing(cx);
  if (!JS_GetProperty(cx, object, "buffer", &backing) ||
      !backing.isObject() ||
      !JS::IsArrayBufferObject(&backing.toObject())) {
    return false;
  }
  buffer.set(&backing.toObject());
  return true;
}

// The streaming compiler consumes on a background thread. Copy the bytes
// while GC is disabled, then hand them to that thread, as the shell's
// ConsumeBufferSource does.
bool FeedBytes(JSContext* cx, JS::StreamConsumer* consumer,
               JS::HandleObject buffer) {
  mozilla::Vector<uint8_t, 0, js::SystemAllocPolicy> bytes;
  {
    JS::AutoCheckCannotGC nogc(cx);
    bool isShared = false;
    const uint8_t* data = JS::GetArrayBufferData(buffer, &isShared, nogc);
    size_t length = JS::GetArrayBufferByteLength(buffer);
    if (!data || length == 0) {
      return RejectStream(cx, consumer, kErrorResponse,
                          "Empty WebAssembly module");
    }
    if (!bytes.resize(length)) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
    std::memcpy(bytes.begin(), data, length);
  }
  struct StreamJob {
    mozilla::Vector<uint8_t, 0, js::SystemAllocPolicy> bytes;
    JS::StreamConsumer* consumer;
  };
  auto* job = new StreamJob{std::move(bytes), consumer};
  std::thread([job]() {
    job->consumer->consumeChunk(job->bytes.begin(), job->bytes.length());
    job->consumer->streamEnd();
    delete job;
  }).detach();
  return true;
}

bool ConsumeArrayBufferSource(JSContext* cx, JS::HandleObject object,
                              JS::StreamConsumer* consumer) {
  JS::RootedObject buffer(cx);
  if (!AsArrayBuffer(cx, object, &buffer)) {
    return RejectStream(cx, consumer, kErrorResponse,
                        "WebAssembly streaming requires a Response or "
                        "BufferSource");
  }
  return FeedBytes(cx, consumer, buffer);
}

bool ReadBoolean(JSContext* cx, JS::HandleObject object, const char* name,
                 bool* result) {
  JS::RootedValue value(cx);
  if (!JS_GetProperty(cx, object, name, &value)) return false;
  *result = value.isBoolean() && value.toBoolean();
  return true;
}

// Validate the parts of fetch's Response contract that Blink checks before
// streaming: 2xx status, exact application/wasm MIME, and an unconsumed body.
bool ValidateResponse(JSContext* cx, JS::HandleObject response,
                      JS::StreamConsumer* consumer) {
  JS::RootedValue ok(cx), status(cx);
  if (!JS_GetProperty(cx, response, "ok", &ok) ||
      !JS_GetProperty(cx, response, "status", &status)) {
    return false;
  }
  if (!ok.isBoolean() || !ok.toBoolean()) {
    return RejectStream(cx, consumer, kErrorResponse,
                        "HTTP status code is not ok");
  }

  JS::RootedValue headers(cx);
  if (!JS_GetProperty(cx, response, "headers", &headers) ||
      !headers.isObject()) {
    return RejectStream(cx, consumer, kErrorResponse,
                        "Incorrect response MIME type. Expected "
                        "'application/wasm'.");
  }
  JS::RootedObject headerObject(cx, &headers.toObject());
  JS::RootedValue get(cx);
  if (!JS_GetProperty(cx, headerObject, "get", &get) || !get.isObject()) {
    return RejectStream(cx, consumer, kErrorResponse,
                        "Incorrect response MIME type. Expected "
                        "'application/wasm'.");
  }
  JS::RootedString name(cx, JS_NewStringCopyZ(cx, "Content-Type"));
  JS::RootedValue nameValue(cx, JS::StringValue(name));
  JS::RootedValue mimeValue(cx);
  if (!JS_CallFunctionValue(cx, headerObject, get,
                            JS::HandleValueArray(nameValue), &mimeValue)) {
    return false;
  }
  std::string mime;
  if (mimeValue.isString()) {
    JS::RootedString mimeString(cx, mimeValue.toString());
    JS::UniqueChars chars = JS_EncodeStringToUTF8(cx, mimeString);
    if (!chars) return false;
    mime = chars.get();
  }
  for (char& c : mime) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  if (mime != "application/wasm") {
    return RejectStream(cx, consumer, kErrorResponse,
                        "Incorrect response MIME type. Expected "
                        "'application/wasm'.");
  }

  bool used = false;
  if (!ReadBoolean(cx, response, "bodyUsed", &used)) return false;
  JS::RootedValue body(cx);
  if (!JS_GetProperty(cx, response, "body", &body)) return false;
  bool locked = false;
  JS::RootedObject bodyObject(cx, body.isObject() ? &body.toObject() : nullptr);
  if (bodyObject && !ReadBoolean(cx, bodyObject, "locked", &locked)) {
    return false;
  }
  if (used || locked) {
    return RejectStream(
        cx, consumer, kErrorResponse,
        "Cannot compile WebAssembly.Module from an already read Response");
  }
  return true;
}

// Feed a ruyijs Response body directly. Its BodyRecord owns the bytes, so
// no Promise reactions are needed (which would nest JS execution inside the
// wasm callback's own Promise reaction and deadlock the await).
bool ConsumeResponseBody(JSContext* cx, JS::HandleObject response,
                         JS::StreamConsumer* consumer) {
  resource_network::BodyRecord* body = resource_network::BodyOf(response);
  if (!body) {
    return RejectStream(cx, consumer, kErrorResponse,
                        "WebAssembly streaming requires a Response or "
                        "BufferSource");
  }
  JS::RootedValue responseValue(cx, JS::ObjectValue(*response));
  JS::RootedObject holder(cx, resource_network::BodyHolderOf(responseValue));
  if (holder && resource_network::BodyUsed(holder)) {
    return RejectStream(
        cx, consumer, kErrorResponse,
        "Cannot compile WebAssembly.Module from an already read Response");
  }
  mozilla::Vector<uint8_t, 0, js::SystemAllocPolicy> bytes;
  if (!body->bytes.empty() && !bytes.resize(body->bytes.size())) {
    JS_ReportOutOfMemory(cx);
    return false;
  }
  if (!body->bytes.empty()) {
    std::memcpy(bytes.begin(), body->bytes.data(), body->bytes.size());
  }
  body->used = true;
  if (bytes.empty()) {
    return RejectStream(cx, consumer, kErrorResponse,
                        "Empty WebAssembly module");
  }
  struct StreamJob {
    mozilla::Vector<uint8_t, 0, js::SystemAllocPolicy> bytes;
    JS::StreamConsumer* consumer;
  };
  auto* job = new StreamJob{std::move(bytes), consumer};
  std::thread([job]() {
    job->consumer->consumeChunk(job->bytes.begin(), job->bytes.length());
    job->consumer->streamEnd();
    delete job;
  }).detach();
  return true;
}

bool ConsumeStream(JSContext* cx, JS::HandleObject object, JS::MimeType,
                   JS::StreamConsumer* consumer) {
  JS::RootedValue arrayBufferMethod(cx);
  if (!JS_GetProperty(cx, object, "arrayBuffer", &arrayBufferMethod)) {
    return false;
  }
  if (!arrayBufferMethod.isObject()) {
    return ConsumeArrayBufferSource(cx, object, consumer);
  }
  if (!ValidateResponse(cx, object, consumer)) return false;
  return ConsumeResponseBody(cx, object, consumer);
}

}  // namespace

void InitForContext(JSContext* cx) {
  JS::InitConsumeStreamCallback(cx, ConsumeStream, ReportStreamError);
}

}  // namespace wasm_stream
}  // namespace ruyijs

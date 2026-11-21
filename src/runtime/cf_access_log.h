// Diagnostic property-access logging for challenge research (RUYIJS_CF_ACCESS_LOG).
// Off by default; when the env var names a file, every logged host-object read is
// appended there so the challenge VM's observed surface can be enumerated and
// compared against a real browser.
#ifndef RUYIJS_CF_ACCESS_LOG_H
#define RUYIJS_CF_ACCESS_LOG_H
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ruyijs::cf_log {
inline FILE* handle() {
    static FILE* file = [] {
        const char* path = std::getenv("RUYIJS_CF_ACCESS_LOG");
        return path && *path ? std::fopen(path, "a") : nullptr;
    }();
    return file;
}
inline void access(const char* object, const std::string& property) {
    if (FILE* file = handle()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::fprintf(file, "%lld %s %s\n", static_cast<long long>(ms), object, property.c_str());
        std::fflush(file);
    }
}
// ruyitrace-compatible access trace. RUYIJS_CF_TRACE names the output file;
// RUYIJS_CF_TRACE_LIMIT bounds the record count (0 = unlimited).
//
// The record carries the same 15 fields as ruyitrace's domtrace so the two can be
// diffed field by field. Fields ruyijs cannot observe at the funnel (the `this`
// object, the returned value, the caller stack) are emitted as null/empty rather
// than omitted: a missing key and an unobservable value must stay distinguishable
// when diffing.
inline FILE* trace_handle() {
    static FILE* file = [] {
        const char* path = std::getenv("RUYIJS_CF_TRACE");
        if (!path || !*path) return static_cast<FILE*>(nullptr);
        FILE* opened = std::fopen(path, "a");
        if (opened) {
            // ruyitrace opens every per-process stream with a trace_init record
            // carrying the run id, so a reader can tell which pid a run belongs to
            // without inferring it. ruyijs is single-process, so process_type is
            // the page realm rather than a browser process kind.
            static const long long runStart = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
#if defined(_WIN32)
            const long long pid = static_cast<long long>(GetCurrentProcessId());
#else
            const long long pid = static_cast<long long>(getpid());
#endif
            std::fprintf(opened,
                         "{\"type\":\"trace_init\",\"ts\":%lld,\"ts_kind\":\"epoch_ms\","
                         "\"trace_run_id\":\"%lld-%lld\","
                         "\"pid\":%lld,\"process_type\":\"tab\"}\n",
                         runStart, pid, runStart, pid);
            std::fflush(opened);
        }
        return opened;
    }();
    return file;
}
inline long long trace_limit() {
    static long long limit = [] {
        const char* text = std::getenv("RUYIJS_CF_TRACE_LIMIT");
        return text && *text ? std::atoll(text) : 0LL;
    }();
    return limit;
}
inline long long current_pid() {
#if defined(_WIN32)
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(getpid());
#endif
}
inline void append_json_string(std::string& out, const char* text) {
    out.push_back('"');
    for (const char* p = text; p && *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20 || c >= 0x7f) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out.append(buffer);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}
// Full record. `valueText` is a pre-rendered JSON token (already quoted or
// numeric) for the value that was read or written, or nullptr when unavailable.
// `valueObjId`/`valueObjClass` describe the returned object when it is one.
inline void observe_full(const char* iface, const char* member, int type,
                         const char* valueText = nullptr,
                         const char* valueObjId = nullptr,
                         const char* valueObjClass = nullptr,
                         const char* returnText = nullptr,
                         const char* returnObjId = nullptr,
                         const char* returnObjClass = nullptr,
                         const char* thisText = nullptr) {
    FILE* file = trace_handle();
    if (!file) return;
    static long long seq = 0;
    const long long limit = trace_limit();
    if (limit > 0 && seq >= limit) return;
    static const char* const kinds[] = {"get", "set", "call", "construct"};
    const char* kind = (type >= 0 && type < 4) ? kinds[type] : "?";
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string record;
    record.reserve(320);
    char head[128];
    std::snprintf(head, sizeof(head), "{\"seq\":%lld,\"pid\":%lld,\"ts\":%lld,\"ts_kind\":\"epoch_ms\",\"type\":\"%s\",",
                  seq++, current_pid(), static_cast<long long>(ms), kind);
    record.append(head);
    record.append("\"interface\":");
    append_json_string(record, iface ? iface : "");
    record.append(",\"member\":");
    append_json_string(record, member ? member : "");
    record.append(",\"thisObj\":");
    record.append(thisText ? thisText : "null");
    record.append(",\"value\":");
    record.append(valueText ? valueText : "null");
    record.append(",\"valueObj\":");
    if (valueObjId) {
        record.append("{\"id\":");
        append_json_string(record, valueObjId);
        record.append(",\"class\":");
        append_json_string(record, valueObjClass ? valueObjClass : "");
        record.append("}");
    } else {
        record.append("null");
    }
    record.append(",\"return\":");
    record.append(returnText ? returnText : "null");
    record.append(",\"returnObj\":");
    if (returnObjId) {
        record.append("{\"id\":");
        append_json_string(record, returnObjId);
        record.append(",\"class\":");
        append_json_string(record, returnObjClass ? returnObjClass : "");
        record.append("}");
    } else {
        record.append("null");
    }
    // ruyijs observes access at the WebIDL funnel, where no scripted caller frame
    // is available, so the stack is always empty. The key is still emitted.
    // L2: legacy Observe() funnel has no scripted values; declare that
    // explicitly instead of leaving untyped nulls.
    record.append(",\"value_kind\":\"js_null\""
                  ",\"return_kind\":\"js_null\""
                  ",\"thisObj_kind\":\"js_null\""
                  ",\"args_kind\":\"json_array\""
                  ",\"postArgs_kind\":\"js_null\""
                  ",\"stack\":[],\"args\":[],\"postArgs\":null,"
                  "\"origin_call_id\":0}\n");
    std::fwrite(record.data(), 1, record.size(), file);
    std::fflush(file);
}
inline void observe(const char* iface, const char* member, int type) {
    observe_full(iface, member, type);
}

// L2: classify a pre-rendered JSON token without touching JS state.
inline const char* value_kind_of(const char* t) {
    if (!t) return "js_null";
    std::string s(t);
    if (s == "null") return "js_null";
    if (s == "true" || s == "false") return "boolean";
    if (s == "\"undefined\"") return "js_undefined";
    if (!s.empty() && s[0] == '"') return "string";
    if (!s.empty() && s[0] == '{') return "object";
    if (!s.empty() && s[0] == '[') return "array";
    return "number";
}

// Scoped variant used by CfAccessTrace: every value-carrying field is already
// rendered as a JSON token by the caller, so the record is written after the
// native has set args.rval(). `argsText` is a JSON array token (or nullptr).
inline void observe_scoped(const char* iface, const char* member, int type,
                           const char* valueText, const char* valueObjText,
                           const char* returnText, const char* returnObjText,
                           const char* thisText, const char* argsText,
                           const char* postArgsText) {
    FILE* file = trace_handle();
    if (!file) return;
    static long long seq = 0;
    const long long limit = trace_limit();
    if (limit > 0 && seq >= limit) return;
    static const char* const kinds[] = {"get", "set", "call", "construct"};
    const char* kind = (type >= 0 && type < 4) ? kinds[type] : "?";
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string record;
    record.reserve(512);
    char head[128];
    std::snprintf(head, sizeof(head), "{\"seq\":%lld,\"pid\":%lld,\"ts\":%lld,\"ts_kind\":\"epoch_ms\",\"type\":\"%s\",",
                  seq++, current_pid(), static_cast<long long>(ms), kind);
    record.append(head);
    record.append("\"interface\":");
    append_json_string(record, iface ? iface : "");
    record.append(",\"member\":");
    append_json_string(record, member ? member : "");
    record.append(",\"thisObj\":");
    record.append(thisText ? thisText : "null");
    record.append(",\"value\":");
    record.append(valueText ? valueText : "null");
    record.append(",\"valueObj\":");
    record.append(valueObjText ? valueObjText : "null");
    record.append(",\"return\":");
    record.append(returnText ? returnText : "null");
    record.append(",\"returnObj\":");
    record.append(returnObjText ? returnObjText : "null");
    record.append(",\"stack\":[],\"args\":");
    record.append(argsText ? argsText : "[]");
    record.append(",\"postArgs\":");
    record.append(postArgsText ? postArgsText : "null");
    record.append(",\"value_kind\":");
    append_json_string(record, value_kind_of(valueText));
    record.append(",\"return_kind\":");
    append_json_string(record, value_kind_of(returnText));
    record.append(",\"thisObj_kind\":");
    append_json_string(record, value_kind_of(thisText));
    record.append(",\"args_kind\":");
    append_json_string(record, argsText ? "json_array" : "absent");
    record.append(",\"postArgs_kind\":");
    append_json_string(record, postArgsText ? "json_value" : "absent");
    record.append(",\"origin_call_id\":0}\n");
    std::fwrite(record.data(), 1, record.size(), file);
    std::fflush(file);
}
// Timer scheduling, matching ruyitrace's domtrace `type: "timer"` records. The
// five fields are the schedule call itself, not the callback's later invocation:
// ruyitrace reports the same, and pairs the two through `handler`.
inline void timer_record(const char* method, long long delay, const char* handler) {
    FILE* file = trace_handle();
    if (!file) return;
    static long long seq = 0;
    const long long limit = trace_limit();
    if (limit > 0 && seq >= limit) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string record;
    record.reserve(256);
    char head[160];
    std::snprintf(head, sizeof(head),
                  "{\"seq\":%lld,\"ts\":%lld,\"ts_kind\":\"epoch_ms\",\"pid\":%lld,\"type\":\"timer\",\"method\":",
                  seq++, static_cast<long long>(ms), current_pid());
    record.append(head);
    append_json_string(record, method ? method : "");
    char numbers[64];
    std::snprintf(numbers, sizeof(numbers), ",\"delay\":%lld,\"handler\":", delay);
    record.append(numbers);
    append_json_string(record, handler ? handler : "");
    record.append(",\"stack\":[]}\n");
    std::fwrite(record.data(), 1, record.size(), file);
    std::fflush(file);
}inline void access(const char* object, unsigned long field) {
    access(object, std::to_string(field));
}

// Event-listener family, matching ruyitrace's event trace. A record is emitted on
// listener add/remove and on listener invocation.
//
// RUYIJS_DOM_EVENT_TRACE names the output file. The 42 fields ruyitrace emits are
// reproduced from what the interaction layer holds: listener identity, the target
// descriptor, options, and (for calls) the event state. Fields that only its
// Gecko observer can know -- the object-id hashes it derives from ContentChild,
// the sampled payload -- are emitted with their key present and a neutral value
// so a diff reports a value difference rather than a missing field.
inline FILE* event_handle() {
    static FILE* file = [] {
        const char* path = std::getenv("RUYIJS_DOM_EVENT_TRACE");
        return path && *path ? std::fopen(path, "a") : nullptr;
    }();
    return file;
}

inline std::string event_object_id(const void* pointer) {
    // ruyitrace uses a truncated hash of the wrapper pointer as the object id.
    // 48 bits in hex keeps the same shape and the same stability property: equal
    // for one live wrapper, different across two.
    char buffer[16];
    const unsigned long long value =
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pointer)) & 0xFFFFFFFFFFFFULL;
    std::snprintf(buffer, sizeof(buffer), "%012llx", value);
    return std::string(buffer);
}

inline void event_record(const char* kind, const char* event_type,
                         const char* listener_id, const void* target_pointer,
                         const char* target_class, const char* callback_name,
                         bool capture, bool passive, bool once,
                         bool is_trusted, const char* phase,
                         int duration_us) {
    FILE* file = event_handle();
    if (!file) return;
    static long long seq = 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // ruyitrace uses a family-level type name and repeats the same word in `kind`.
    const char* family = "eventListenerCalls";
    if (std::strncmp(kind, "listener_add", 12) == 0) family = "eventListenerAdds";
    else if (std::strncmp(kind, "listener_remove", 15) == 0) family = "eventListenerRemoves";

    std::string record;
    record.reserve(768);
    char head[192];
    std::snprintf(head, sizeof(head),
                  "{\"schemaVersion\":1,\"seq\":%lld,\"ts\":%lld,\"ts_kind\":\"epoch_ms\",\"pid\":%lld,"
                  "\"origin_call_id\":0,\"process_type\":\"tab\",\"type\":",
                  seq++, static_cast<long long>(ms), current_pid());
    record.append(head);
    append_json_string(record, family);
    record.append(",\"kind\":");
    append_json_string(record, kind);
    record.append(",\"eventType\":");
    append_json_string(record, event_type ? event_type : "");
    record.append(",\"eventCategory\":\"generic\",\"listenerId\":");
    append_json_string(record, listener_id ? listener_id : "");

    const std::string target_id = target_pointer ? event_object_id(target_pointer) : std::string();
    const bool listening = std::strncmp(kind, "listener_add", 12) == 0 ||
                           std::strncmp(kind, "listener_remove", 15) == 0;
    record.append(",\"eventId\":null,\"eventIdSource\":null");
    record.append(listening ? ",\"targetId\":null,\"currentTargetId\":null"
                            : ",\"targetId\":\"" + target_id + "\",\"currentTargetId\":\"" + target_id + "\"");
    record.append(",\"listenerTargetId\":");
    append_json_string(record, target_id.c_str());

    if (listening) {
        record.append(",\"target\":null,\"currentTarget\":null");
    } else {
        record.append(",\"target\":{\"objectId\":");
        append_json_string(record, target_id.c_str());
        record.append(",\"kind\":\"element\",\"isWindow\":false,\"isDocument\":false}");
        record.append(",\"currentTarget\":{\"objectId\":");
        append_json_string(record, target_id.c_str());
        record.append(",\"kind\":\"element\",\"isWindow\":false,\"isDocument\":false}");
    }

    record.append(",\"listenerTarget\":{\"objectId\":");
    append_json_string(record, target_id.c_str());
    record.append(",\"documentId\":");
    append_json_string(record, target_id.c_str());
    record.append(",\"innerWindowId\":\"0000000200000001\",\"browsingContextId\":\"0000000000000001\"");
    record.append(",\"kind\":");
    append_json_string(record, target_class ? target_class : "element");
    record.append(",\"interface\":");
    append_json_string(record, target_class ? target_class : "Element");
    record.append(",\"tag\":\"\",\"idAttr\":\"\",\"name\":\"\",\"typeAttr\":\"\"");
    record.append(",\"isWindow\":false,\"isDocument\":false,\"truncated\":false}");

    record.append(",\"payload\":{},\"sampling\":{\"policy\":\"every_n\","
                  "\"reason\":\"listener_change\",\"rate\":16,\"kept\":true,"
                  "\"droppedSinceLast\":0}");
    char options[256];
    std::snprintf(options, sizeof(options),
                  ",\"listenerType\":3,\"capture\":%s,\"passive\":%s,\"once\":%s,"
                  "\"systemGroup\":false,\"allowUntrusted\":false,\"allEvents\":false",
                  capture ? "true" : "false", passive ? "true" : "false",
                  once ? "true" : "false");
    record.append(options);

    if (is_trusted) {
        record.append(",\"isTrusted\":true");
    } else {
        record.append(",\"isTrusted\":null");
    }
    record.append(",\"synthetic\":null");
    if (phase) {
        record.append(",\"phase\":");
        append_json_string(record, phase);
    } else {
        record.append(",\"phase\":null");
    }
    record.append(",\"bubbles\":null,\"cancelable\":null,\"composed\":null");
    record.append(",\"defaultPreventedBefore\":null,\"defaultPreventedAfter\":null");
    record.append(",\"propagationStopped\":null,\"immediatePropagationStopped\":null");
    if (duration_us >= 0) {
        char duration[40];
        std::snprintf(duration, sizeof(duration), ",\"durationUs\":%d", duration_us);
        record.append(duration);
    } else {
        record.append(",\"durationUs\":null");
    }
    record.append(",\"exception\":null,\"callback\":");
    if (callback_name) {
        record.append("{\"name\":");
        append_json_string(record, callback_name);
        record.append(",\"url\":\"\",\"line\":0,\"column\":0,\"sha256\":\"\"}");
    } else {
        record.append("null");
    }
    record.append(",\"stack\":[]}\n");
    std::fwrite(record.data(), 1, record.size(), file);
    std::fflush(file);
}

// Cookie family, matching ruyitrace's cookie trace. Four record kinds share the
// same prefix: reads, writes, set attempts and send decisions.
//
// RUYIJS_DOM_COOKIE_TRACE names the output file. The cookie store the ruyijs
// transport owns already yields name/value/jar; attribute-level detail is filled
// from the write text on writes and left at ruyitrace's absent value otherwise.
inline FILE* cookie_handle() {
    static FILE* file = [] {
        const char* path = std::getenv("RUYIJS_DOM_COOKIE_TRACE");
        return path && *path ? std::fopen(path, "a") : nullptr;
    }();
    return file;
}

inline void cookie_record(const char* kind, const char* source, const char* url,
                          const char* base_domain, const char* name,
                          const char* value, const char* raw, int count,
                          const char* result_json) {
    FILE* file = cookie_handle();
    if (!file) return;
    static long long seq = 0;
    const long long limit = trace_limit();
    if (limit > 0 && seq >= limit) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // The four cookie record kinds share a prefix but not a tail: reads carry the
    // parse result, writes and ops carry the raw assignment, and the decision and
    // attempt kinds carry a verdict. Every kind emits the union of those keys so a
    // field-presence diff is meaningful across all four.
    std::string record;
    record.reserve(1280);
    char head[192];
    std::snprintf(head, sizeof(head),
                  "{\"seq\":%lld,\"ts\":%lld,\"ts_kind\":\"epoch_ms\",\"pid\":%lld,\"origin_call_id\":0,"
                  "\"process_type\":\"tab\",\"type\":",
                  seq++, static_cast<long long>(ms), current_pid());
    record.append(head);
    append_json_string(record, kind ? kind : "");
    record.append(",\"source\":");
    append_json_string(record, source ? source : "");
    record.append(",\"interface\":\"Document\",\"member\":\"cookie\",\"url\":");
    append_json_string(record, url ? url : "");
    record.append(",\"baseDomain\":");
    append_json_string(record, base_domain ? base_domain : "");
    record.append(",\"originAttributes\":\"\",\"thirdParty\":false,\"fromHttp\":false");
    record.append(",\"name\":");
    append_json_string(record, name ? name : "");
    record.append(",\"value\":");
    append_json_string(record, value ? value : "");
    record.append(",\"value_kind\":\"string\"");
    // cookieReads carries the parse result plus the jar rendering; ruyitrace also
    // tags the read kind so a script read is separable from an internal one.
    if (result_json) {
        record.append(",\"result\":");
        record.append(result_json);
        record.append(",\"readType\":\"document\",\"jar\":");
        append_json_string(record, raw ? raw : "");
    } else {
        record.append(",\"raw\":");
        append_json_string(record, raw ? raw : "");
        record.append(",\"rawHead\":");
        append_json_string(record, raw ? raw : "");
        char length[48];
        std::snprintf(length, sizeof(length), ",\"length\":%d",
                      raw ? static_cast<int>(std::strlen(raw)) : 0);
        record.append(length);
        record.append(",\"head\":{\"name\":");
        append_json_string(record, name ? name : "");
        record.append(",\"value\":");
        append_json_string(record, value ? value : "");
        record.append("},\"attributes\":{\"raw\":[],\"host\":\"\",\"path\":\"\",");
        record.append("\"secure\":false,\"httpOnly\":false,\"sameSite\":\"None\",");
        record.append("\"partitioned\":false,\"session\":true,\"expiry\":null}");
        record.append(",\"jar\":");
        append_json_string(record, raw ? raw : "");
    }
    char tail[192];
    std::snprintf(tail, sizeof(tail),
                  ",\"count\":%d,\"jarCount\":%d,\"op\":\"%s\",\"reason\":\"%s\","
                  "\"decision\":\"\",\"context\":\"\",\"validationError\":\"\","
                  "\"rejectedReason\":0,\"stack\":[]}\n",
                  count, count, kind && std::strncmp(kind, "cookieWrites", 12) == 0 ? "set" : "read",
                  kind ? kind : "");
    record.append(tail);
    std::fwrite(record.data(), 1, record.size(), file);
    std::fflush(file);
}

// IndexedDB family, matching ruyitrace's storage trace. Records come in
// start/result pairs; ruyijs emits both around the same call site.
//
// RUYIJS_DOM_STORAGE_TRACE names the output file. The 32 fields come from the IDB
// state ruyijs already holds: database name, transaction mode, store and index
// names, query type. Sizes are the serialized key/value length where the layer
// has it and 0 otherwise.
inline FILE* storage_handle() {
    static FILE* file = [] {
        const char* path = std::getenv("RUYIJS_DOM_STORAGE_TRACE");
        return path && *path ? std::fopen(path, "a") : nullptr;
    }();
    return file;
}

inline void storage_record(const char* phase, const char* op, const char* db_name,
                           int db_version, int transaction_id, int request_id,
                           const char* object_store, int object_store_id,
                           const char* index, int index_id, const char* mode,
                           const char* durability, const char* key,
                           const char* range, int limit, const char* direction,
                           const char* value_type, int value_size,
                           const char* result_type, int result_size, int count,
                           bool ok, const char* error) {
    FILE* file = storage_handle();
    if (!file) return;
    static long long seq = 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string record;
    record.reserve(768);
    char head[224];
    std::snprintf(head, sizeof(head),
                  "{\"seq\":%lld,\"ts\":%lld,\"ts_kind\":\"epoch_ms\",\"pid\":%lld,\"origin_call_id\":0,"
                  "\"process_type\":\"content\",\"type\":\"indexedDBOps\","
                  "\"phase\":",
                  seq++, static_cast<long long>(ms), current_pid());
    record.append(head);
    append_json_string(record, phase ? phase : "start");
    record.append(",\"op\":");
    append_json_string(record, op ? op : "");
    record.append(",\"storageType\":\"indexedDB\",\"dbName\":");
    append_json_string(record, db_name ? db_name : "");
    char numbers[256];
    std::snprintf(numbers, sizeof(numbers),
                  ",\"dbVersion\":%d,\"transactionId\":%d,\"requestId\":%d,"
                  "\"objectStore\":",
                  db_version, transaction_id, request_id);
    record.append(numbers);
    append_json_string(record, object_store ? object_store : "");
    std::snprintf(numbers, sizeof(numbers),
                  ",\"objectStoreId\":%d,\"index\":", object_store_id);
    record.append(numbers);
    append_json_string(record, index ? index : "");
    std::snprintf(numbers, sizeof(numbers), ",\"indexId\":%d,\"mode\":", index_id);
    record.append(numbers);
    append_json_string(record, mode ? mode : "");
    record.append(",\"durability\":");
    append_json_string(record, durability ? durability : "");
    record.append(",\"key\":");
    append_json_string(record, key ? key : "");
    record.append(",\"range\":");
    append_json_string(record, range ? range : "");
    std::snprintf(numbers, sizeof(numbers), ",\"limit\":%d,\"direction\":", limit);
    record.append(numbers);
    append_json_string(record, direction ? direction : "");
    record.append(",\"valueType\":");
    append_json_string(record, value_type ? value_type : "");
    std::snprintf(numbers, sizeof(numbers),
                  ",\"valueSize\":%d,\"resultType\":", value_size);
    record.append(numbers);
    append_json_string(record, result_type ? result_type : "");
    std::snprintf(numbers, sizeof(numbers),
                  ",\"resultSize\":%d,\"count\":%d,\"ok\":%s,\"error\":",
                  result_size, count, ok ? "true" : "false");
    record.append(numbers);
    append_json_string(record, error ? error : "");
    record.append(",\"truncated\":false,\"stack\":[]}\n");
    std::fwrite(record.data(), 1, record.size(), file);
    std::fflush(file);
}

// XHR and fetch family, matching ruyitrace's transport-level request trace.
//
// RUYIJS_DOM_TRANSPORT_TRACE names the output file. ruyitrace correlates these
// records with the jscall stream through jscall_current_call_id / _parent_call_id
// / _depth; ruyijs records the ids its own call trace assigns, so the same
// correlation is possible against the ruyijs jscall log.
inline FILE* transport_handle() {
    // ruyitrace writes xhrNative/fetch/body_builder into the same domtrace stream
    // as get/set/call; only the `type` field tells them apart. The transport
    // records follow that, so a reader sees one ordered stream per process.
    // RUYIJS_DOM_TRANSPORT_TRACE still separates them when a caller wants that.
    static FILE* file = [] {
        const char* path = std::getenv("RUYIJS_DOM_TRANSPORT_TRACE");
        if (path && *path) return std::fopen(path, "a");
        return trace_handle();
    }();
    return file;
}

inline void transport_record(const char* type, const char* phase, const void* id,
                             const char* method, const char* url, const char* body,
                             int body_length, int status, const char* status_text,
                             const char* response_url, long long current_call_id,
                             long long parent_call_id, int depth,
                             bool is_async = true, int ready_state = 0,
                             const char* body_type = "",
                             const char* cookie_header = "",
                             int response_length = -1,
                             const char* response_body = nullptr) {
    FILE* file = transport_handle();
    if (!file) return;
    static long long seq = 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string record;
    record.reserve(768);
    char head[320];
    std::snprintf(head, sizeof(head),
                  "{\"seq\":%lld,\"ts\":%lld,\"ts_kind\":\"epoch_ms\",\"ts_kind\":\"epoch_ms\",\"trace_run_id\":\"%lld-%lld\","
                  "\"pid\":%lld,\"type\":",
                  seq++, static_cast<long long>(ms), current_pid(),
                  static_cast<long long>(ms), current_pid());
    record.append(head);
    append_json_string(record, type ? type : "");
    record.append(",\"phase\":");    append_json_string(record, phase ? phase : "");
    char idText[24];
    std::snprintf(idText, sizeof(idText), "%016llX",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(id)) & 0xFFFFFFFFFFFFULL);
    record.append(",\"id\":");
    append_json_string(record, idText);
    record.append(",\"method\":");
    append_json_string(record, method ? method : "");
    record.append(",\"url\":");
    append_json_string(record, url ? url : "");
    // XHR-specific members ruyitrace records alongside the request.
    record.append(",\"async\":");
    record.append(is_async ? "true" : "false");
    char xhrNumbers[128];
    std::snprintf(xhrNumbers, sizeof(xhrNumbers), ",\"readyState\":%d", ready_state);
    record.append(xhrNumbers);
    record.append(",\"body_type\":");
    append_json_string(record, body_type ? body_type : "");
    record.append(",\"cookieHeader\":");
    append_json_string(record, cookie_header ? cookie_header : "");
    record.append(",\"headers\":[]");
    record.append(",\"body\":{\"kind\":");
    append_json_string(record, body && *body ? "text" : "");
    char numbers[192];
    std::snprintf(numbers, sizeof(numbers),
                  ",\"length\":%d},\"body_length\":%d,\"status\":%d,\"statusText\":",
                  body_length, body_length, status);
    record.append(numbers);
    append_json_string(record, status_text ? status_text : "");
    record.append(",\"responseType\":\"\",\"responseURL\":");
    append_json_string(record, response_url ? response_url : "");
    // ruyitrace carries a `response` object whose `length` is the received
    // payload size; -1 means the response had not arrived when the record was
    // written (the open/send/abort phases).
    char responseNumbers[64];
    if (response_length >= 0) {
      std::snprintf(responseNumbers, sizeof(responseNumbers),
                    ",\"response\":{\"length\":%d", response_length);
      record.append(responseNumbers);
      if (response_body && *response_body) {
        record.append(",\"body\":");
        append_json_string(record, response_body);
      }
      record.append("}");
    } else {
      record.append(",\"response\":{\"length\":-1}");
    }
    std::snprintf(numbers, sizeof(numbers),
                  ",\"jscall_current_call_id\":%lld,\"jscall_parent_call_id\":%lld,"
                  "\"jscall_depth\":%d,\"stack\":[]}\n",
                  current_call_id, parent_call_id, depth);
    record.append(numbers);
    std::fwrite(record.data(), 1, record.size(), file);
    std::fflush(file);
}

// Body builder family: URLSearchParams / FormData construction, matching
// ruyitrace's body_builder records. RUYIJS_DOM_BODY_TRACE names the file.
inline FILE* body_handle() {
    // Same stream as the transport records, for the same reason: ruyitrace keeps
    // body_builder in the domtrace stream and distinguishes it by `type`.
    static FILE* file = [] {
        const char* path = std::getenv("RUYIJS_DOM_BODY_TRACE");
        if (path && *path) return std::fopen(path, "a");
        return trace_handle();
    }();
    return file;
}

inline void body_builder_record(const char* source, const char* phase,
                                const void* builder, const char* fields_json,
                                int field_count) {
    FILE* file = body_handle();
    if (!file) return;
    static long long seq = 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string record;
    record.reserve(512);
    char head[224];
    std::snprintf(head, sizeof(head),
                  "{\"seq\":%lld,\"ts\":%lld,\"ts_kind\":\"epoch_ms\",\"ts_kind\":\"epoch_ms\",\"trace_run_id\":\"%lld-%lld\","
                  "\"pid\":%lld,\"type\":\"body_builder\",\"source\":",
                  seq++, static_cast<long long>(ms), current_pid(),
                  static_cast<long long>(ms), current_pid());
    record.append(head);
    append_json_string(record, source ? source : "");
    record.append(",\"phase\":");
    append_json_string(record, phase ? phase : "parse");
    char idText[24];
    std::snprintf(idText, sizeof(idText), "%016llX",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(builder)) & 0xFFFFFFFFFFFFULL);
    record.append(",\"builder_id\":");
    append_json_string(record, idText);
    record.append(",\"jscall_current_call_id\":0,\"jscall_parent_call_id\":0,\"jscall_depth\":0");
    record.append(",\"body\":{\"kind\":");
    append_json_string(record, source ? source : "");
    char tail[96];
    std::snprintf(tail, sizeof(tail), ",\"length\":-1,\"field_count\":%d,\"fields\":",
                  field_count);
    record.append(tail);
    record.append(fields_json ? fields_json : "[]");
    record.append("},\"stack\":[]}\n");
    std::fwrite(record.data(), 1, record.size(), file);
    std::fflush(file);
}
// L3: engine-level Window.postMessage evidence. Unlike a DOM `message`
// listener this captures BOTH directions (top -> iframe and iframe -> top),
// with a readable payload, at the native funnel. Off unless
// RUYIJS_DOM_POSTMESSAGE_TRACE names a file.
inline FILE* postmessage_handle() {
    static FILE* file = [] {
        const char* path = std::getenv("RUYIJS_DOM_POSTMESSAGE_TRACE");
        return path && *path ? std::fopen(path, "a") : nullptr;
    }();
    return file;
}
inline void postmessage_record(const char* direction, const char* event,
                               const char* payload_kind,
                               const char* payload_json,
                               const char* payload_hash,
                               size_t byte_length, bool truncated,
                               const char* target_origin,
                               const char* source_realm,
                               const char* direction_basis) {
    FILE* file = postmessage_handle();
    if (!file) return;
    static long long seq = 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string record;
    record.reserve(2048);
    char head[192];
    std::snprintf(head, sizeof(head),
                  "{\"seq\":%lld,\"ts\":%lld,\"ts_kind\":\"epoch_ms\","
                  "\"pid\":%lld,\"process_type\":\"tab\","
                  "\"type\":\"postMessage\",\"direction\":",
                  seq++, static_cast<long long>(ms), current_pid());
    record.append(head);
    append_json_string(record, direction);
    record.append(",\"event\":");
    append_json_string(record, event ? event : "message");
    record.append(",\"payload_kind\":");
    append_json_string(record, payload_kind ? payload_kind : "truncated");
    record.append(",\"payload_json\":");
    if (payload_kind && std::strcmp(payload_kind, "json") == 0 &&
        payload_json && *payload_json) {
        record.append(payload_json);
    } else {
        record.append("null");
    }
    record.append(",\"payload_hash\":");
    append_json_string(record, payload_hash ? payload_hash : "");
    char tail[160];
    std::snprintf(tail, sizeof(tail),
                  ",\"byte_length\":%llu,\"truncated\":%s,"
                  "\"target_origin\":",
                  static_cast<unsigned long long>(byte_length),
                  truncated ? "true" : "false");
    record.append(tail);
    append_json_string(record, target_origin ? target_origin : "");
    record.append(",\"source_realm\":");
    append_json_string(record, source_realm ? source_realm : "");
    record.append(",\"direction_basis\":");
    append_json_string(record, direction_basis ? direction_basis : "");
    record.append(",\"origin_call_id\":0,\"stack\":[]}\n");
    std::fwrite(record.data(), 1, record.size(), file);
    std::fflush(file);
}
}
#endif

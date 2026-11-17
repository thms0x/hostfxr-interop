#ifndef NATIVE_INTEROP_ABI_H
#define NATIVE_INTEROP_ABI_H
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define NI_CALL __cdecl
#define NI_EXPORT __declspec(dllexport)
#else
#define NI_CALL
#define NI_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C"
{
#endif
#define NI_ABI_VERSION 1u
    enum ni_kind
    {
        NI_NULL,
        NI_BOOL,
        NI_I32,
        NI_I64,
        NI_U32,
        NI_U64,
        NI_F32,
        NI_F64,
        NI_UTF8,
        NI_OBJECT,
        NI_ARRAY,
        NI_I8,
        NI_U8,
        NI_I16,
        NI_U16,
        NI_CHAR16
    };
    enum ni_operation
    {
        NI_LOAD_TYPE = 1,
        NI_CONSTRUCT,
        NI_CALL_STATIC,
        NI_CALL_METHOD,
        NI_GET_STATIC,
        NI_SET_STATIC,
        NI_GET,
        NI_SET,
        NI_RELEASE,
        NI_LIVE_HANDLES
    };
    /* Requests borrow storage for the duration of invoke. Results own their buffers
       and object leases. Strings are UTF-8 with byte lengths, including embedded NUL.
       Object scalars are opaque lease IDs, never native addresses. Reserved = 0. */
    typedef struct ni_value
    {
        int32_t kind, reserved;
        uint64_t scalar;
        const void *data;
        int64_t length;
    } ni_value;
    typedef struct ni_request
    {
        uint32_t size, version;
        int32_t operation, reserved;
        uint64_t target;
        ni_value type_name, name, arguments;
    } ni_request;
    typedef struct ni_result
    {
        ni_value value, error_type, error_message;
    } ni_result;
    /* Never throw across the ABI. Always free results, even on failure. To adopt an
       object lease, zero its scalar before free_result (also inside arrays).
       free_result releases unadopted leases and zeros the result; repeated calls are safe. */
    typedef int32_t(NI_CALL *ni_invoke_fn)(const ni_request *, ni_result *);
    typedef void(NI_CALL *ni_free_result_fn)(ni_result *);
    typedef struct ni_api
    {
        uint32_t size, version;
        ni_invoke_fn invoke;
        ni_free_result_fn free_result;
    } ni_api;
    typedef int32_t(NI_CALL *ni_get_api_fn)(uint32_t, uint32_t, ni_api *);
    /* Each plugin exports exactly this C entry point. */
    int32_t NI_CALL ni_get_api(uint32_t version, uint32_t size, ni_api *output);
#ifdef __cplusplus
}
static_assert(sizeof(void *) == 8, "Native interop ABI requires a 64-bit process");
static_assert(sizeof(ni_value) == 32 && offsetof(ni_value, data) == 16);
static_assert(sizeof(ni_request) == 120 && sizeof(ni_result) == 96);
static_assert(sizeof(ni_api) == 24);
#endif
#endif

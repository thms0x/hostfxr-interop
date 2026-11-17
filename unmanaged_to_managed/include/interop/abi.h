#ifndef INTEROP_ABI_H
#define INTEROP_ABI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define INTEROP_CALLCONV __cdecl
#else
#define INTEROP_CALLCONV
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#define INTEROP_ABI_VERSION 2u

    enum interop_kind
    {
        INTEROP_NULL,
        INTEROP_BOOL,
        INTEROP_I32,
        INTEROP_I64,
        INTEROP_U32,
        INTEROP_U64,
        INTEROP_F32,
        INTEROP_F64,
        INTEROP_UTF8,
        INTEROP_OBJECT,
        INTEROP_ARRAY,
        INTEROP_I8,
        INTEROP_U8,
        INTEROP_I16,
        INTEROP_U16,
        INTEROP_CHAR16
    };

    enum interop_operation
    {
        INTEROP_LOAD_TYPE = 1,
        INTEROP_CONSTRUCT,
        INTEROP_CALL_STATIC,
        INTEROP_CALL,
        INTEROP_GET_STATIC,
        INTEROP_SET_STATIC,
        INTEROP_GET,
        INTEROP_SET,
        INTEROP_RELEASE,
        INTEROP_DISPOSE,
        INTEROP_LIVE_HANDLES
    };

    /* UTF-8 and array storage is borrowed in requests and owned in results.
       scalar stores integers, IEEE float bits, or an opaque GCHandle lease value.
       Object handles are valid only until their matching INTEROP_RELEASE call;
       callers must never replay a released handle or invent a handle value.
       data/length describe UTF-8 bytes or interop_value elements; no NUL terminator.
       Reserved fields must be zero. ABI v2 supports 64-bit processes only. */
    typedef struct interop_value
    {
        int32_t kind;
        int32_t reserved;
        uint64_t scalar;
        const void *data;
        int64_t length;
    } interop_value;

    typedef struct interop_request
    {
        uint32_t size;
        uint32_t version;
        int32_t operation;
        int32_t reserved;
        uint64_t target;
        interop_value name;
        interop_value assembly;
        interop_value arguments;
        interop_value parameter_types; /* null = infer; array (even empty) = exact */
        interop_value generic_types;
    } interop_request;

    typedef struct interop_result
    {
        interop_value value;
        interop_value error_type;
        interop_value error_message;
        interop_value error_stack;
    } interop_result;

    /* No exceptions may cross these functions. Nonzero invoke status is an error.
       Always free a result, including errors. To adopt a returned object lease,
       zero its scalar before free_result. This applies recursively to arrays.
       free_result releases any unadopted leases and zeros the result. */
    typedef int32_t(INTEROP_CALLCONV *interop_invoke_fn)(const interop_request *, interop_result *);
    typedef void(INTEROP_CALLCONV *interop_free_result_fn)(interop_result *);

    typedef struct interop_api
    {
        uint32_t size;
        uint32_t version;
        interop_invoke_fn invoke;
        interop_free_result_fn free_result;
    } interop_api;

    typedef int32_t(INTEROP_CALLCONV *interop_get_api_fn)(uint32_t, uint32_t, interop_api *);

#ifdef __cplusplus
}
static_assert(sizeof(void *) == 8, "interop ABI v2 requires a 64-bit process");
static_assert(sizeof(interop_value) == 32);
static_assert(offsetof(interop_value, data) == 16);
static_assert(sizeof(interop_request) == 184);
static_assert(sizeof(interop_result) == 128);
static_assert(sizeof(interop_api) == 24);
#endif
#endif

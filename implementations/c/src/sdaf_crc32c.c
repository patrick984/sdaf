#include "sdaf_internal.h"

#include <stdio.h>

uint32_t sdaf_crc32c(const void* data, size_t size)
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t crc = UINT32_MAX;
    size_t i;
    for (i = 0u; i < size; ++i) {
        unsigned bit;
        crc ^= bytes[i];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ ((crc & 1u) != 0u ? UINT32_C(0x82f63b78) : 0u);
    }
    return crc ^ UINT32_MAX;
}

uint16_t sdaf_get_u16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u));
}
uint32_t sdaf_get_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u)
        | ((uint32_t)p[3] << 24u);
}
uint64_t sdaf_get_u64(const uint8_t* p)
{
    return (uint64_t)sdaf_get_u32(p) | ((uint64_t)sdaf_get_u32(p + 4u) << 32u);
}
int32_t sdaf_get_i32(const uint8_t* p)
{
    uint32_t u = sdaf_get_u32(p);
    int32_t v;
    memcpy(&v, &u, sizeof(v));
    return v;
}
int64_t sdaf_get_i64(const uint8_t* p)
{
    uint64_t u = sdaf_get_u64(p);
    int64_t v;
    memcpy(&v, &u, sizeof(v));
    return v;
}
double sdaf_get_f64(const uint8_t* p)
{
    uint64_t u = sdaf_get_u64(p);
    double v;
    memcpy(&v, &u, sizeof(v));
    return v;
}
void sdaf_put_u16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8u);
}
void sdaf_put_u32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8u);
    p[2] = (uint8_t)(v >> 16u);
    p[3] = (uint8_t)(v >> 24u);
}
void sdaf_put_u64(uint8_t* p, uint64_t v)
{
    sdaf_put_u32(p, (uint32_t)v);
    sdaf_put_u32(p + 4u, (uint32_t)(v >> 32u));
}
void sdaf_put_i32(uint8_t* p, int32_t v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    sdaf_put_u32(p, u);
}
void sdaf_put_i64(uint8_t* p, int64_t v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    sdaf_put_u64(p, u);
}

int sdaf_all_zero(const uint8_t* p, size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i)
        if (p[i] != 0u)
            return 0;
    return 1;
}
int sdaf_add_size(size_t a, size_t b, size_t* result)
{
    if (a > SIZE_MAX - b)
        return 0;
    *result = a + b;
    return 1;
}
int sdaf_mul_size(size_t a, size_t b, size_t* result)
{
    if (a != 0u && b > SIZE_MAX / a)
        return 0;
    *result = a * b;
    return 1;
}
void* sdaf_calloc_array(size_t count, size_t element_size)
{
    size_t total;
    if (!sdaf_mul_size(count, element_size, &total))
        return NULL;
    return calloc(total == 0u ? 1u : count, element_size);
}
uint8_t* sdaf_memdup(const uint8_t* source, size_t size)
{
    uint8_t* p = (uint8_t*)malloc(size == 0u ? 1u : size);
    if (p != NULL && size != 0u)
        memcpy(p, source, size);
    return p;
}
char* sdaf_strndup_bytes(const uint8_t* source, size_t size)
{
    char* p;
    if (size == SIZE_MAX)
        return NULL;
    p = (char*)malloc(size + 1u);
    if (p != NULL) {
        if (size != 0u)
            memcpy(p, source, size);
        p[size] = '\0';
    }
    return p;
}

int sdaf_utf8_valid(const uint8_t* p, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        uint32_t code;
        unsigned continuation;
        uint8_t c = p[i++];
        if (c < 0x80u)
            continue;
        if (c >= 0xc2u && c <= 0xdfu) {
            code = (uint32_t)(c & 0x1fu);
            continuation = 1u;
        } else if (c >= 0xe0u && c <= 0xefu) {
            code = (uint32_t)(c & 0x0fu);
            continuation = 2u;
        } else if (c >= 0xf0u && c <= 0xf4u) {
            code = (uint32_t)(c & 0x07u);
            continuation = 3u;
        } else
            return 0;
        if ((size - i) < continuation)
            return 0;
        while (continuation-- != 0u) {
            uint8_t d = p[i++];
            if ((d & 0xc0u) != 0x80u)
                return 0;
            code = (code << 6u) | (uint32_t)(d & 0x3fu);
        }
        if (code > UINT32_C(0x10ffff) || (code >= UINT32_C(0xd800) && code <= UINT32_C(0xdfff)))
            return 0;
        if ((code < 0x80u && c >= 0xc0u) || (code < 0x800u && c >= 0xe0u)
            || (code < 0x10000u && c >= 0xf0u))
            return 0;
    }
    return 1;
}

void sdaf_set_error(char* buffer, size_t buffer_size, const char* format, ...)
{
    va_list args;
    if (buffer_size == 0u)
        return;
    va_start(args, format);
    (void)vsnprintf(buffer, buffer_size, format, args);
    va_end(args);
}

void sdaf_limits_default(sdaf_limits* limits)
{
    if (limits == NULL)
        return;
    limits->max_header_size = 64u * 1024u;
    limits->max_payload_size = UINT64_C(1024) * 1024u * 1024u;
    limits->max_decoded_size = UINT64_C(4) * 1024u * 1024u * 1024u;
    limits->max_schema_objects = 65535u;
    limits->max_channels_per_stream = 4096u;
    limits->max_utf8_size = 16u * 1024u * 1024u;
    limits->max_records = 1000000u;
}

const char* sdaf_status_string(sdaf_status status)
{
    switch (status) {
    case SDAF_OK:
        return "ok";
    case SDAF_ERROR_ARGUMENT:
        return "invalid argument";
    case SDAF_ERROR_MEMORY:
        return "out of memory";
    case SDAF_ERROR_IO:
        return "I/O error";
    case SDAF_ERROR_FORMAT:
        return "malformed SDAF";
    case SDAF_ERROR_LIMIT:
        return "configured limit exceeded";
    case SDAF_ERROR_UNSUPPORTED:
        return "unsupported feature";
    }
    return "unknown error";
}

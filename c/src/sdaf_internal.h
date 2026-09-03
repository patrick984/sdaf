#ifndef SDAF_INTERNAL_H
#define SDAF_INTERNAL_H

#include "sdaf/sdaf.h"

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define SDAF_TRANSFORM_DELTA 1u
#define SDAF_TRANSFORM_ZIGZAG 2u
#define SDAF_TRANSFORM_SHUFFLE 3u
#define SDAF_TRANSFORM_ZSTD 16u

uint16_t sdaf_get_u16(const uint8_t *p);
uint32_t sdaf_get_u32(const uint8_t *p);
uint64_t sdaf_get_u64(const uint8_t *p);
int32_t sdaf_get_i32(const uint8_t *p);
int64_t sdaf_get_i64(const uint8_t *p);
double sdaf_get_f64(const uint8_t *p);
void sdaf_put_u16(uint8_t *p, uint16_t v);
void sdaf_put_u32(uint8_t *p, uint32_t v);
void sdaf_put_u64(uint8_t *p, uint64_t v);
void sdaf_put_i32(uint8_t *p, int32_t v);
void sdaf_put_i64(uint8_t *p, int64_t v);
int sdaf_all_zero(const uint8_t *p, size_t size);
int sdaf_utf8_valid(const uint8_t *p, size_t size);
int sdaf_add_size(size_t a, size_t b, size_t *result);
int sdaf_mul_size(size_t a, size_t b, size_t *result);
void *sdaf_calloc_array(size_t count, size_t element_size);
uint8_t *sdaf_memdup(const uint8_t *source, size_t size);
char *sdaf_strndup_bytes(const uint8_t *source, size_t size);

const sdaf_schema *sdaf_find_schema(const sdaf_document *doc, uint32_t id, uint32_t revision);
sdaf_status sdaf_validate_schema(const sdaf_schema *schema, const sdaf_limits *limits, char *error, size_t error_size);
void sdaf_free_schema(sdaf_schema *schema);
void sdaf_free_transforms(sdaf_transform *transforms, size_t count);

typedef struct sdaf_channel_info {
    const sdaf_schema_object *object;
    uint32_t id;
    const char *name;
    const char *unit;
    uint8_t logical_type;
    uint16_t logical_bits;
    uint16_t storage_bits;
    uint32_t elements;
    double scale;
    double offset;
} sdaf_channel_info;

sdaf_status sdaf_channels_for_stream(const sdaf_schema *schema, uint32_t stream_id, const sdaf_limits *limits, sdaf_channel_info **channels, size_t *count, size_t *lanes, char *error, size_t error_size);
sdaf_status sdaf_expected_payload_size(const sdaf_channel_info *channels, size_t channel_count, uint32_t sample_count, uint32_t timestamp_bytes, uint8_t packing, size_t *result);
sdaf_status sdaf_decode_data_samples(sdaf_data_record *data, const sdaf_schema *schema, const sdaf_limits *limits, char *error, size_t error_size);
sdaf_status sdaf_decode_transforms(const uint8_t *encoded, size_t encoded_size, const sdaf_transform *transforms, size_t transform_count, const sdaf_channel_info *channels, size_t channel_count, size_t lane_count, const sdaf_data_record *data, uint8_t **decoded, size_t *decoded_size, char *error, size_t error_size);
sdaf_status sdaf_encode_numeric(const uint8_t *canonical, size_t canonical_size, const sdaf_channel_info *channels, size_t channel_count, size_t lane_count, const sdaf_data_info *info, uint8_t **encoded, size_t *encoded_size, char *error, size_t error_size);
sdaf_status sdaf_zstd_compress(const uint8_t *source, size_t source_size, uint8_t **output, size_t *output_size, char *error, size_t error_size);
sdaf_status sdaf_zstd_decompress(const uint8_t *source, size_t source_size, size_t expected_size, uint8_t **output, char *error, size_t error_size);

sdaf_status sdaf_encoder_append(sdaf_encoder *encoder, const void *data, size_t size);
sdaf_status sdaf_encoder_record(sdaf_encoder *encoder, uint16_t type, uint16_t type_flags, const uint8_t *type_header, size_t type_header_size, const uint8_t *payload, size_t payload_size, int trailer_crc);
void sdaf_set_error(char *buffer, size_t buffer_size, const char *format, ...);

#endif

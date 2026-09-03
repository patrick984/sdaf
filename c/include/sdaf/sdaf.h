#ifndef SDAF_SDAF_H
#define SDAF_SDAF_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDAF_VERSION_MAJOR 1u
#define SDAF_VERSION_MINOR 0u
#define SDAF_FILE_HEADER_SIZE 64u
#define SDAF_RECORD_HEADER_SIZE 32u

typedef enum sdaf_status {
    SDAF_OK = 0,
    SDAF_ERROR_ARGUMENT,
    SDAF_ERROR_MEMORY,
    SDAF_ERROR_IO,
    SDAF_ERROR_FORMAT,
    SDAF_ERROR_LIMIT,
    SDAF_ERROR_UNSUPPORTED
} sdaf_status;

typedef enum sdaf_record_type {
    SDAF_RECORD_SCHEMA = 0x0001,
    SDAF_RECORD_DATA = 0x0002,
    SDAF_RECORD_TEXT = 0x0003,
    SDAF_RECORD_INDEX = 0x0004,
    SDAF_RECORD_END = 0x0005,
    SDAF_RECORD_BLOB = 0x0006,
    SDAF_RECORD_NOTE = 0x7fff
} sdaf_record_type;

typedef enum sdaf_object_kind {
    SDAF_OBJECT_FILE_METADATA = 1,
    SDAF_OBJECT_STREAM = 2,
    SDAF_OBJECT_CHANNEL = 3,
    SDAF_OBJECT_CLOCK = 4,
    SDAF_OBJECT_VALUE_MAP = 5,
    SDAF_OBJECT_BITFIELD = 6
} sdaf_object_kind;

typedef enum sdaf_wire_type {
    SDAF_WIRE_U8 = 1,
    SDAF_WIRE_U16 = 2,
    SDAF_WIRE_U32 = 3,
    SDAF_WIRE_U64 = 4,
    SDAF_WIRE_I64 = 5,
    SDAF_WIRE_F64 = 6,
    SDAF_WIRE_UTF8 = 7,
    SDAF_WIRE_BYTES = 8,
    SDAF_WIRE_RATIONAL_U64 = 9,
    SDAF_WIRE_BOOL = 10,
    SDAF_WIRE_RATIONAL_I64 = 11
} sdaf_wire_type;

typedef enum sdaf_logical_type {
    SDAF_LOGICAL_UNSIGNED = 1,
    SDAF_LOGICAL_SIGNED = 2,
    SDAF_LOGICAL_FLOAT = 3,
    SDAF_LOGICAL_BOOLEAN = 4,
    SDAF_LOGICAL_FIXED_BYTES = 5
} sdaf_logical_type;

typedef enum sdaf_timestamp_mode {
    SDAF_TIMESTAMP_PERIODIC = 1,
    SDAF_TIMESTAMP_DELTA = 2,
    SDAF_TIMESTAMP_EXPLICIT = 3,
    SDAF_TIMESTAMP_NONE = 4
} sdaf_timestamp_mode;

typedef enum sdaf_layout {
    SDAF_LAYOUT_INTERLEAVED = 1,
    SDAF_LAYOUT_PLANAR = 2
} sdaf_layout;

typedef enum sdaf_packing {
    SDAF_PACKING_LSB0_DENSE = 1,
    SDAF_PACKING_BYTE_ALIGNED = 2
} sdaf_packing;

typedef enum sdaf_compression {
    SDAF_COMPRESSION_NONE = 0,
    SDAF_COMPRESSION_ZSTANDARD = 1,
    SDAF_COMPRESSION_NUMERIC = 2
} sdaf_compression;

typedef struct sdaf_limits {
    size_t max_header_size;
    uint64_t max_payload_size;
    uint64_t max_decoded_size;
    uint32_t max_schema_objects;
    uint32_t max_channels_per_stream;
    size_t max_utf8_size;
    size_t max_records;
} sdaf_limits;

typedef struct sdaf_file_header {
    uint8_t major;
    uint8_t minor;
    int64_t created_unix_ns;
    uint8_t file_uuid[16];
    uint64_t first_record_offset;
} sdaf_file_header;

typedef struct sdaf_tlv {
    uint16_t tag;
    uint8_t wire_type;
    uint32_t size;
    uint8_t *value;
} sdaf_tlv;

typedef struct sdaf_schema_object {
    uint8_t kind;
    uint32_t id;
    size_t tlv_count;
    sdaf_tlv *tlvs;
} sdaf_schema_object;

typedef struct sdaf_schema {
    uint32_t id;
    uint32_t revision;
    size_t object_count;
    sdaf_schema_object *objects;
} sdaf_schema;

typedef struct sdaf_transform {
    uint16_t id;
    uint8_t version;
    uint32_t parameter_size;
    uint8_t *parameters;
} sdaf_transform;

typedef struct sdaf_sample_value {
    uint32_t channel_id;
    const char *channel_name;
    uint32_t element_index;
    uint8_t logical_type;
    uint64_t raw_unsigned;
    int64_t raw_signed;
    double numeric_value;
    double physical_value;
    uint8_t *bytes;
    size_t byte_count;
    const char *unit;
} sdaf_sample_value;

typedef struct sdaf_sample {
    uint64_t index;
    int has_time;
    int64_t time_ticks;
    size_t value_count;
    sdaf_sample_value *values;
} sdaf_sample;

typedef struct sdaf_record_envelope {
    uint16_t type;
    uint16_t flags;
    uint16_t header_size;
    uint8_t version;
    uint32_t sequence;
    uint64_t payload_size;
    uint64_t file_offset;
    int payload_crc_in_trailer;
} sdaf_record_envelope;

typedef struct sdaf_data_record {
    uint32_t schema_id;
    uint32_t schema_revision;
    uint32_t stream_id;
    uint32_t sample_count;
    uint64_t first_sample_index;
    int64_t start_time_ticks;
    uint64_t period_numerator;
    uint64_t period_denominator;
    uint8_t timestamp_mode;
    uint8_t layout;
    uint8_t packing;
    uint32_t timestamp_bytes;
    uint64_t decoded_sample_bytes;
    size_t transform_count;
    sdaf_transform *transforms;
    uint8_t *stored_payload;
    size_t stored_payload_size;
    uint8_t *decoded_payload;
    size_t decoded_payload_size;
    sdaf_sample *samples;
} sdaf_data_record;

typedef struct sdaf_text_record {
    uint32_t schema_id;
    uint32_t schema_revision;
    uint32_t stream_id;
    int64_t time_ticks;
    uint8_t severity;
    int has_source_id;
    uint32_t source_id;
    int has_event_code;
    int32_t event_code;
    char *message;
    size_t message_size;
} sdaf_text_record;

typedef struct sdaf_blob_record {
    uint32_t schema_id;
    uint32_t schema_revision;
    uint32_t stream_id;
    uint64_t item_index;
    int64_t time_ticks;
    uint64_t decoded_bytes;
    size_t transform_count;
    sdaf_transform *transforms;
    uint8_t *stored_payload;
    size_t stored_payload_size;
    uint8_t *decoded_payload;
    size_t decoded_payload_size;
} sdaf_blob_record;

typedef struct sdaf_index_entry {
    uint64_t record_offset;
    uint32_t sequence;
    uint32_t stream_id;
    uint64_t first_sample_index;
    uint32_t sample_count;
    int64_t first_time_ticks;
    int64_t last_time_ticks;
} sdaf_index_entry;

typedef struct sdaf_record {
    sdaf_record_envelope envelope;
    union {
        sdaf_schema schema;
        sdaf_data_record data;
        sdaf_text_record text;
        sdaf_blob_record blob;
        struct { size_t entry_count; sdaf_index_entry *entries; } index;
        struct { uint64_t total_records; uint64_t total_data_records; uint64_t last_index_offset; } end;
        struct { char *message; size_t size; } note;
        struct { uint8_t *type_header; size_t type_header_size; uint8_t *payload; size_t payload_size; } unknown;
    } value;
} sdaf_record;

typedef struct sdaf_document {
    sdaf_file_header header;
    size_t record_count;
    sdaf_record *records;
    sdaf_status status;
    uint64_t error_offset;
    char error[192];
} sdaf_document;

typedef struct sdaf_data_info {
    uint32_t schema_id;
    uint32_t schema_revision;
    uint32_t stream_id;
    uint32_t sample_count;
    uint64_t first_sample_index;
    int64_t start_time_ticks;
    uint64_t period_numerator;
    uint64_t period_denominator;
    uint8_t timestamp_mode;
    uint8_t layout;
    uint8_t packing;
    uint32_t timestamp_bytes;
    uint8_t compression;
    int payload_crc_in_trailer;
} sdaf_data_info;

typedef struct sdaf_text_info {
    uint32_t schema_id;
    uint32_t schema_revision;
    uint32_t stream_id;
    int64_t time_ticks;
    uint8_t severity;
    int has_source_id;
    uint32_t source_id;
    int has_event_code;
    int32_t event_code;
    int payload_crc_in_trailer;
} sdaf_text_info;

typedef struct sdaf_blob_info {
    uint32_t schema_id;
    uint32_t schema_revision;
    uint32_t stream_id;
    uint64_t item_index;
    int64_t time_ticks;
    uint8_t compression;
    int payload_crc_in_trailer;
} sdaf_blob_info;

typedef struct sdaf_encoder {
    uint8_t *data;
    size_t size;
    size_t capacity;
    uint32_t next_sequence;
    uint64_t record_count;
    uint64_t data_record_count;
    sdaf_status status;
    char error[192];
} sdaf_encoder;

void sdaf_limits_default(sdaf_limits *limits);
const char *sdaf_status_string(sdaf_status status);
uint32_t sdaf_crc32c(const void *data, size_t size);

sdaf_status sdaf_decode(const void *data, size_t size, const sdaf_limits *limits, sdaf_document *document);
sdaf_status sdaf_decode_file(const char *path, const sdaf_limits *limits, sdaf_document *document);
void sdaf_document_free(sdaf_document *document);

const sdaf_tlv *sdaf_object_find_tlv(const sdaf_schema_object *object, uint16_t tag, size_t occurrence);
const sdaf_schema_object *sdaf_schema_find_object(const sdaf_schema *schema, uint8_t kind, uint32_t id);
int sdaf_tlv_u8(const sdaf_tlv *tlv, uint8_t *value);
int sdaf_tlv_u16(const sdaf_tlv *tlv, uint16_t *value);
int sdaf_tlv_u32(const sdaf_tlv *tlv, uint32_t *value);
int sdaf_tlv_u64(const sdaf_tlv *tlv, uint64_t *value);
int sdaf_tlv_i64(const sdaf_tlv *tlv, int64_t *value);
int sdaf_tlv_f64(const sdaf_tlv *tlv, double *value);

sdaf_status sdaf_encoder_init(sdaf_encoder *encoder, int64_t created_unix_ns, const uint8_t file_uuid[16]);
void sdaf_encoder_free(sdaf_encoder *encoder);
sdaf_status sdaf_encoder_write_schema(sdaf_encoder *encoder, const sdaf_schema *schema, int trailer_crc);
sdaf_status sdaf_encoder_write_data(sdaf_encoder *encoder, const sdaf_schema *schema, const sdaf_data_info *info, const void *canonical_payload, size_t payload_size);
sdaf_status sdaf_encoder_write_text(sdaf_encoder *encoder, const sdaf_text_info *info, const char *message, size_t message_size);
sdaf_status sdaf_encoder_write_blob(sdaf_encoder *encoder, const sdaf_blob_info *info, const void *decoded_payload, size_t payload_size);
sdaf_status sdaf_encoder_write_note(sdaf_encoder *encoder, const char *message, size_t message_size, int trailer_crc);
sdaf_status sdaf_encoder_write_index(sdaf_encoder *encoder, const sdaf_index_entry *entries, size_t entry_count, int trailer_crc);
sdaf_status sdaf_encoder_write_end(sdaf_encoder *encoder, uint64_t last_index_offset);
sdaf_status sdaf_encoder_write_private(sdaf_encoder *encoder, uint16_t record_type, const void *type_header, size_t type_header_size, const void *payload, size_t payload_size, int trailer_crc);
sdaf_status sdaf_encoder_write_file(const sdaf_encoder *encoder, const char *path);

#ifdef __cplusplus
}
#endif

#endif

#include "sdaf/sdaf.h"
#include "unity.h"

#include <string.h>

static uint8_t u32_stream_one[] = {1u, 0u, 0u, 0u};
static uint8_t type_unsigned[] = {SDAF_LOGICAL_UNSIGNED};
static uint8_t type_signed[] = {SDAF_LOGICAL_SIGNED};
static uint8_t type_float[] = {SDAF_LOGICAL_FLOAT};
static uint8_t type_bool[] = {SDAF_LOGICAL_BOOLEAN};
static uint8_t type_fixed[] = {SDAF_LOGICAL_FIXED_BYTES};
static uint8_t bits_1[] = {1u, 0u};
static uint8_t bits_8[] = {8u, 0u};
static uint8_t bits_12[] = {12u, 0u};
static uint8_t bits_16[] = {16u, 0u};
static uint8_t bits_32[] = {32u, 0u};
static uint8_t bits_128[] = {128u, 0u};

static void put_i64(uint8_t *p, int64_t value)
{
    uint64_t u; unsigned i; memcpy(&u, &value, sizeof(u)); for (i = 0u; i < 8u; ++i) p[i] = (uint8_t)(u >> (i * 8u));
}

static sdaf_document encode_decode(const sdaf_schema *schema, const sdaf_data_info *info, const uint8_t *payload, size_t size, sdaf_status *status)
{
    sdaf_encoder encoder; sdaf_document document;
    memset(&document, 0, sizeof(document));
    *status = sdaf_encoder_init(&encoder, INT64_MIN, NULL);
    if (*status == SDAF_OK) *status = sdaf_encoder_write_schema(&encoder, schema, 0);
    if (*status == SDAF_OK) *status = sdaf_encoder_write_data(&encoder, schema, info, payload, size);
    if (*status == SDAF_OK) *status = sdaf_decode(encoder.data, encoder.size, NULL, &document);
    sdaf_encoder_free(&encoder); return document;
}

void test_byte_aligned_mixed_types_and_wide_fixed_bytes(void)
{
    static uint8_t count[] = {4u, 0u, 0u, 0u};
    static sdaf_tlv stream[] = {{104u, SDAF_WIRE_U32, 4u, count}};
    static sdaf_tlv signed_channel[] = {{1u, SDAF_WIRE_UTF8, 6u, (uint8_t *)"signed"}, {200u, SDAF_WIRE_U32, 4u, u32_stream_one}, {201u, SDAF_WIRE_U8, 1u, type_signed}, {202u, SDAF_WIRE_U16, 2u, bits_12}, {203u, SDAF_WIRE_U16, 2u, bits_16}};
    static sdaf_tlv float_channel[] = {{1u, SDAF_WIRE_UTF8, 5u, (uint8_t *)"float"}, {200u, SDAF_WIRE_U32, 4u, u32_stream_one}, {201u, SDAF_WIRE_U8, 1u, type_float}, {202u, SDAF_WIRE_U16, 2u, bits_32}, {203u, SDAF_WIRE_U16, 2u, bits_32}};
    static sdaf_tlv bool_channel[] = {{1u, SDAF_WIRE_UTF8, 4u, (uint8_t *)"flag"}, {200u, SDAF_WIRE_U32, 4u, u32_stream_one}, {201u, SDAF_WIRE_U8, 1u, type_bool}, {202u, SDAF_WIRE_U16, 2u, bits_1}, {203u, SDAF_WIRE_U16, 2u, bits_1}};
    static sdaf_tlv fixed_channel[] = {{1u, SDAF_WIRE_UTF8, 5u, (uint8_t *)"bytes"}, {200u, SDAF_WIRE_U32, 4u, u32_stream_one}, {201u, SDAF_WIRE_U8, 1u, type_fixed}, {203u, SDAF_WIRE_U16, 2u, bits_128}};
    static sdaf_schema_object objects[] = {{SDAF_OBJECT_STREAM, 1u, 1u, stream}, {SDAF_OBJECT_CHANNEL, 1u, 5u, signed_channel}, {SDAF_OBJECT_CHANNEL, 2u, 5u, float_channel}, {SDAF_OBJECT_CHANNEL, 3u, 5u, bool_channel}, {SDAF_OBJECT_CHANNEL, 4u, 4u, fixed_channel}};
    static sdaf_schema schema = {20u, 1u, 5u, objects};
    uint8_t payload[23] = {0xffu, 0xffu, 0x00u, 0x00u, 0xc0u, 0x3fu, 1u}; sdaf_data_info info; sdaf_document document; sdaf_status status; size_t i;
    for (i = 0u; i < 16u; ++i) payload[7u + i] = (uint8_t)i;
    memset(&info, 0, sizeof(info)); info.schema_id = 20u; info.schema_revision = 1u; info.stream_id = 1u; info.sample_count = 1u; info.timestamp_mode = SDAF_TIMESTAMP_NONE; info.layout = SDAF_LAYOUT_INTERLEAVED; info.packing = SDAF_PACKING_BYTE_ALIGNED;
    document = encode_decode(&schema, &info, payload, sizeof(payload), &status); TEST_ASSERT_EQUAL_INT(SDAF_OK, status); TEST_ASSERT_EQUAL_INT64(-1, document.records[1].value.data.samples[0].values[0].raw_signed); TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.5, document.records[1].value.data.samples[0].values[1].numeric_value); TEST_ASSERT_EQUAL_UINT64(1u, document.records[1].value.data.samples[0].values[2].raw_unsigned); TEST_ASSERT_EQUAL_UINT8_ARRAY(payload + 7u, document.records[1].value.data.samples[0].values[3].bytes, 16u); sdaf_document_free(&document);
}

static sdaf_schema two_u8_schema(void)
{
    static uint8_t count[] = {2u, 0u, 0u, 0u}; static sdaf_tlv stream[] = {{104u, SDAF_WIRE_U32, 4u, count}};
    static sdaf_tlv a[] = {{1u, SDAF_WIRE_UTF8, 1u, (uint8_t *)"a"}, {200u, SDAF_WIRE_U32, 4u, u32_stream_one}, {201u, SDAF_WIRE_U8, 1u, type_unsigned}, {202u, SDAF_WIRE_U16, 2u, bits_8}, {203u, SDAF_WIRE_U16, 2u, bits_8}};
    static sdaf_tlv b[] = {{1u, SDAF_WIRE_UTF8, 1u, (uint8_t *)"b"}, {200u, SDAF_WIRE_U32, 4u, u32_stream_one}, {201u, SDAF_WIRE_U8, 1u, type_unsigned}, {202u, SDAF_WIRE_U16, 2u, bits_8}, {203u, SDAF_WIRE_U16, 2u, bits_8}};
    static sdaf_schema_object objects[] = {{SDAF_OBJECT_STREAM, 1u, 1u, stream}, {SDAF_OBJECT_CHANNEL, 1u, 5u, a}, {SDAF_OBJECT_CHANNEL, 2u, 5u, b}}; sdaf_schema result = {21u, 1u, 3u, objects}; return result;
}

void test_planar_layout_reassembles_samples(void)
{
    const uint8_t payload[] = {1u, 2u, 10u, 20u}; sdaf_schema schema = two_u8_schema(); sdaf_data_info info; sdaf_document document; sdaf_status status;
    memset(&info, 0, sizeof(info)); info.schema_id = 21u; info.schema_revision = 1u; info.stream_id = 1u; info.sample_count = 2u; info.timestamp_mode = SDAF_TIMESTAMP_NONE; info.layout = SDAF_LAYOUT_PLANAR; info.packing = SDAF_PACKING_BYTE_ALIGNED;
    document = encode_decode(&schema, &info, payload, sizeof(payload), &status); TEST_ASSERT_EQUAL_INT(SDAF_OK, status); TEST_ASSERT_EQUAL_UINT64(1u, document.records[1].value.data.samples[0].values[0].raw_unsigned); TEST_ASSERT_EQUAL_UINT64(10u, document.records[1].value.data.samples[0].values[1].raw_unsigned); TEST_ASSERT_EQUAL_UINT64(2u, document.records[1].value.data.samples[1].values[0].raw_unsigned); TEST_ASSERT_EQUAL_UINT64(20u, document.records[1].value.data.samples[1].values[1].raw_unsigned); sdaf_document_free(&document);
}

static sdaf_schema one_u8_schema(void)
{
    static uint8_t count[] = {1u, 0u, 0u, 0u}; static sdaf_tlv stream[] = {{104u, SDAF_WIRE_U32, 4u, count}};
    static sdaf_tlv channel[] = {{1u, SDAF_WIRE_UTF8, 1u, (uint8_t *)"a"}, {200u, SDAF_WIRE_U32, 4u, u32_stream_one}, {201u, SDAF_WIRE_U8, 1u, type_unsigned}, {202u, SDAF_WIRE_U16, 2u, bits_8}, {203u, SDAF_WIRE_U16, 2u, bits_8}};
    static sdaf_schema_object objects[] = {{SDAF_OBJECT_STREAM, 1u, 1u, stream}, {SDAF_OBJECT_CHANNEL, 1u, 5u, channel}}; sdaf_schema result = {22u, 1u, 2u, objects}; return result;
}

static void timestamp_test(uint8_t mode, int64_t first, int64_t second, int64_t expected_first, int64_t expected_second)
{
    uint8_t payload[18]; sdaf_schema schema = one_u8_schema(); sdaf_data_info info; sdaf_document document; sdaf_status status;
    put_i64(payload, first); put_i64(payload + 8u, second); payload[16] = 3u; payload[17] = 4u;
    memset(&info, 0, sizeof(info)); info.schema_id = 22u; info.schema_revision = 1u; info.stream_id = 1u; info.sample_count = 2u; info.start_time_ticks = mode == SDAF_TIMESTAMP_DELTA ? 100 : 0; info.timestamp_mode = mode; info.timestamp_bytes = 16u; info.layout = SDAF_LAYOUT_INTERLEAVED; info.packing = SDAF_PACKING_BYTE_ALIGNED;
    document = encode_decode(&schema, &info, payload, sizeof(payload), &status); TEST_ASSERT_EQUAL_INT(SDAF_OK, status); TEST_ASSERT_EQUAL_INT64(expected_first, document.records[1].value.data.samples[0].time_ticks); TEST_ASSERT_EQUAL_INT64(expected_second, document.records[1].value.data.samples[1].time_ticks); sdaf_document_free(&document);
}

void test_delta_timestamps(void) { timestamp_test(SDAF_TIMESTAMP_DELTA, 0, 7, 100, 107); }
void test_explicit_timestamps(void) { timestamp_test(SDAF_TIMESTAMP_EXPLICIT, -5, 25, -5, 25); }

void test_dense_padding_is_validated(void)
{
    static uint8_t count[] = {1u, 0u, 0u, 0u}; static sdaf_tlv stream[] = {{104u, SDAF_WIRE_U32, 4u, count}};
    static sdaf_tlv channel[] = {{1u, SDAF_WIRE_UTF8, 4u, (uint8_t *)"flag"}, {200u, SDAF_WIRE_U32, 4u, u32_stream_one}, {201u, SDAF_WIRE_U8, 1u, type_bool}, {202u, SDAF_WIRE_U16, 2u, bits_1}, {203u, SDAF_WIRE_U16, 2u, bits_1}};
    static sdaf_schema_object objects[] = {{SDAF_OBJECT_STREAM, 1u, 1u, stream}, {SDAF_OBJECT_CHANNEL, 1u, 5u, channel}}; static sdaf_schema schema = {23u, 1u, 2u, objects}; const uint8_t payload[] = {0xfeu}; sdaf_data_info info; sdaf_document document; sdaf_status status;
    memset(&info, 0, sizeof(info)); info.schema_id = 23u; info.schema_revision = 1u; info.stream_id = 1u; info.sample_count = 1u; info.timestamp_mode = SDAF_TIMESTAMP_NONE; info.layout = SDAF_LAYOUT_INTERLEAVED; info.packing = SDAF_PACKING_LSB0_DENSE;
    document = encode_decode(&schema, &info, payload, sizeof(payload), &status); TEST_ASSERT_EQUAL_INT(SDAF_ERROR_FORMAT, status); sdaf_document_free(&document);
}

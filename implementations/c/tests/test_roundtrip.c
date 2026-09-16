#include "sdaf/sdaf.h"
#include "unity.h"

#include <string.h>

static uint8_t stream_count[] = { 2u, 0u, 0u, 0u };
static uint8_t stream_id[] = { 1u, 0u, 0u, 0u };
static uint8_t unsigned_type[] = { SDAF_LOGICAL_UNSIGNED };
static uint8_t twelve_bits[] = { 12u, 0u };
static sdaf_tlv stream_tlvs[] = { { 104u, SDAF_WIRE_U32, 4u, stream_count } };
static sdaf_tlv channel1_tlvs[] = { { 1u, SDAF_WIRE_UTF8, 4u, (uint8_t*)"adc0" },
    { 200u, SDAF_WIRE_U32, 4u, stream_id }, { 201u, SDAF_WIRE_U8, 1u, unsigned_type },
    { 202u, SDAF_WIRE_U16, 2u, twelve_bits }, { 203u, SDAF_WIRE_U16, 2u, twelve_bits } };
static sdaf_tlv channel2_tlvs[] = { { 1u, SDAF_WIRE_UTF8, 4u, (uint8_t*)"adc1" },
    { 200u, SDAF_WIRE_U32, 4u, stream_id }, { 201u, SDAF_WIRE_U8, 1u, unsigned_type },
    { 202u, SDAF_WIRE_U16, 2u, twelve_bits }, { 203u, SDAF_WIRE_U16, 2u, twelve_bits } };
static sdaf_schema_object objects[]
    = { { SDAF_OBJECT_STREAM, 1u, 1u, stream_tlvs }, { SDAF_OBJECT_CHANNEL, 1u, 5u, channel1_tlvs },
          { SDAF_OBJECT_CHANNEL, 2u, 5u, channel2_tlvs } };
static sdaf_schema schema = { 1u, 1u, 3u, objects };
static const uint8_t packed[] = { 0x00u, 0xf0u, 0xffu, 0x23u, 0x61u, 0x45u, 0xbcu, 0x9au, 0x78u };

static void roundtrip_data(uint8_t compression, int trailer)
{
    static const uint64_t expected[] = { 0u, 4095u, 291u, 1110u, 2748u, 1929u };
    sdaf_encoder encoder;
    sdaf_data_info info;
    sdaf_document document;
    size_t s, v, index = 0u;
    memset(&info, 0, sizeof(info));
    info.schema_id = 1u;
    info.schema_revision = 1u;
    info.stream_id = 1u;
    info.sample_count = 3u;
    info.start_time_ticks = 100;
    info.period_numerator = 2u;
    info.period_denominator = 1u;
    info.timestamp_mode = SDAF_TIMESTAMP_PERIODIC;
    info.layout = SDAF_LAYOUT_INTERLEAVED;
    info.packing = SDAF_PACKING_LSB0_DENSE;
    info.compression = compression;
    info.payload_crc_in_trailer = trailer;
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_init(&encoder, 123, NULL));
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_write_schema(&encoder, &schema, 0));
    TEST_ASSERT_EQUAL_INT(
        SDAF_OK, sdaf_encoder_write_data(&encoder, &schema, &info, packed, sizeof(packed)));
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_write_end(&encoder, 0u));
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_decode(encoder.data, encoder.size, NULL, &document));
    TEST_ASSERT_EQUAL_size_t(3u, document.record_count);
    TEST_ASSERT_EQUAL_INT(trailer, document.records[1].envelope.payload_crc_in_trailer);
    for (s = 0u; s < 3u; ++s) {
        TEST_ASSERT_EQUAL_INT64(
            100 + (int64_t)(s * 2u), document.records[1].value.data.samples[s].time_ticks);
        for (v = 0u; v < 2u; ++v)
            TEST_ASSERT_EQUAL_UINT64(expected[index++],
                document.records[1].value.data.samples[s].values[v].raw_unsigned);
    }
    sdaf_document_free(&document);
    sdaf_encoder_free(&encoder);
}

void test_roundtrip_identity(void) { roundtrip_data(SDAF_COMPRESSION_NONE, 0); }
void test_roundtrip_trailer(void) { roundtrip_data(SDAF_COMPRESSION_NONE, 1); }
void test_roundtrip_zstandard(void) { roundtrip_data(SDAF_COMPRESSION_ZSTANDARD, 0); }
void test_roundtrip_numeric_profile(void) { roundtrip_data(SDAF_COMPRESSION_NUMERIC, 0); }

static void roundtrip_bitwise(const uint8_t* payload, size_t payload_size, const uint64_t* expected,
    uint32_t sample_count, uint32_t channel_count, uint16_t bits)
{
    uint8_t stream_count_bytes[] = { (uint8_t)channel_count, 0u, 0u, 0u };
    uint8_t stream_id_bytes[] = { 3u, 0u, 0u, 0u };
    uint8_t float_type[] = { SDAF_LOGICAL_FLOAT };
    uint8_t width_bytes[] = { (uint8_t)bits, 0u };
    sdaf_tlv float_stream_tlvs[] = { { 104u, SDAF_WIRE_U32, 4u, stream_count_bytes } };
    sdaf_tlv float0_tlvs[] = { { 1u, SDAF_WIRE_UTF8, 2u, (uint8_t*)"f0" },
        { 200u, SDAF_WIRE_U32, 4u, stream_id_bytes }, { 201u, SDAF_WIRE_U8, 1u, float_type },
        { 202u, SDAF_WIRE_U16, 2u, width_bytes }, { 203u, SDAF_WIRE_U16, 2u, width_bytes } };
    sdaf_tlv float1_tlvs[] = { { 1u, SDAF_WIRE_UTF8, 2u, (uint8_t*)"f1" },
        { 200u, SDAF_WIRE_U32, 4u, stream_id_bytes }, { 201u, SDAF_WIRE_U8, 1u, float_type },
        { 202u, SDAF_WIRE_U16, 2u, width_bytes }, { 203u, SDAF_WIRE_U16, 2u, width_bytes } };
    sdaf_schema_object float_objects[] = { { SDAF_OBJECT_STREAM, 3u, 1u, float_stream_tlvs },
        { SDAF_OBJECT_CHANNEL, 1u, 5u, float0_tlvs },
        { SDAF_OBJECT_CHANNEL, 2u, 5u, float1_tlvs } };
    sdaf_schema float_schema = { 3u, 1u, channel_count + 1u, float_objects };
    sdaf_encoder encoder;
    sdaf_data_info info;
    sdaf_document document;
    size_t sample, channel, index = 0u;
    memset(&info, 0, sizeof(info));
    info.schema_id = 3u;
    info.schema_revision = 1u;
    info.stream_id = 3u;
    info.sample_count = sample_count;
    info.timestamp_mode = SDAF_TIMESTAMP_NONE;
    info.layout = SDAF_LAYOUT_INTERLEAVED;
    info.packing = SDAF_PACKING_BYTE_ALIGNED;
    info.compression = SDAF_COMPRESSION_BITWISE;
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_init(&encoder, INT64_MIN, NULL));
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_write_schema(&encoder, &float_schema, 0));
    TEST_ASSERT_EQUAL_INT(
        SDAF_OK, sdaf_encoder_write_data(&encoder, &float_schema, &info, payload, payload_size));
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_decode(encoder.data, encoder.size, NULL, &document));
    TEST_ASSERT_EQUAL_size_t(3u, document.records[1].value.data.transform_count);
    TEST_ASSERT_EQUAL_UINT16(5u, document.records[1].value.data.transforms[0].id);
    TEST_ASSERT_EQUAL_UINT16(3u, document.records[1].value.data.transforms[1].id);
    TEST_ASSERT_EQUAL_UINT16(16u, document.records[1].value.data.transforms[2].id);
    TEST_ASSERT_EQUAL_size_t(payload_size, document.records[1].value.data.decoded_payload_size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        payload, document.records[1].value.data.decoded_payload, payload_size);
    for (sample = 0u; sample < sample_count; ++sample)
        for (channel = 0u; channel < channel_count; ++channel)
            TEST_ASSERT_EQUAL_UINT64(expected[index++],
                document.records[1].value.data.samples[sample].values[channel].raw_unsigned);
    sdaf_document_free(&document);
    sdaf_encoder_free(&encoder);
}

void test_roundtrip_bitwise_f32(void)
{
    static const uint8_t payload[]
        = { 0x00u, 0x00u, 0x80u, 0x3fu, 0x00u, 0x00u, 0x00u, 0xc0u, 0x00u, 0x00u, 0xc0u, 0x3fu,
              0x00u, 0x00u, 0x00u, 0xc0u, 0x00u, 0x00u, 0xc0u, 0x3fu, 0x00u, 0x00u, 0x20u, 0xc0u };
    static const uint64_t expected[] = { UINT64_C(0x3f800000), UINT64_C(0xc0000000),
        UINT64_C(0x3fc00000), UINT64_C(0xc0000000), UINT64_C(0x3fc00000), UINT64_C(0xc0200000) };
    roundtrip_bitwise(payload, sizeof(payload), expected, 3u, 2u, 32u);
}

void test_roundtrip_bitwise_f64(void)
{
    static const uint8_t payload[] = { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x80u,
        0x34u, 0x12u, 0x00u, 0x00u, 0x00u, 0x00u, 0xf8u, 0x7fu, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xf0u, 0x7fu };
    static const uint64_t expected[] = { UINT64_C(0x8000000000000000), UINT64_C(0x7ff8000000001234),
        UINT64_C(0x0000000000000001), UINT64_C(0x7ff0000000000000) };
    roundtrip_bitwise(payload, sizeof(payload), expected, 4u, 1u, 64u);
}

void test_typed_profiles_reject_wrong_channel_kind(void)
{
    sdaf_encoder encoder;
    sdaf_data_info info;
    memset(&info, 0, sizeof(info));
    info.schema_id = 1u;
    info.schema_revision = 1u;
    info.stream_id = 1u;
    info.sample_count = 3u;
    info.timestamp_mode = SDAF_TIMESTAMP_NONE;
    info.layout = SDAF_LAYOUT_INTERLEAVED;
    info.packing = SDAF_PACKING_LSB0_DENSE;
    info.compression = SDAF_COMPRESSION_BITWISE;
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_init(&encoder, INT64_MIN, NULL));
    TEST_ASSERT_EQUAL_INT(SDAF_ERROR_ARGUMENT,
        sdaf_encoder_write_data(&encoder, &schema, &info, packed, sizeof(packed)));
    sdaf_encoder_free(&encoder);
}

void test_roundtrip_text_blob_index_end_private(void)
{
    sdaf_encoder encoder;
    sdaf_text_info text;
    sdaf_blob_info blob;
    sdaf_index_entry entry;
    sdaf_document document;
    uint64_t blob_offset, index_offset;
    static const uint8_t bytes[] = { 1u, 2u, 3u };
    memset(&text, 0, sizeof(text));
    memset(&blob, 0, sizeof(blob));
    memset(&entry, 0, sizeof(entry));
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_init(&encoder, INT64_MIN, NULL));
    text.time_ticks = 9;
    text.severity = 4u;
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_write_text(&encoder, &text, "warning", 7u));
    blob.schema_id = 1u;
    blob.schema_revision = 1u;
    blob.stream_id = 20u;
    blob.item_index = 42u;
    blob.time_ticks = INT64_MIN;
    blob.compression = SDAF_COMPRESSION_ZSTANDARD;
    blob_offset = encoder.size;
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_write_blob(&encoder, &blob, bytes, sizeof(bytes)));
    entry.record_offset = blob_offset;
    entry.sequence = 1u;
    entry.stream_id = 20u;
    entry.first_sample_index = 42u;
    entry.sample_count = 1u;
    entry.first_time_ticks = INT64_MIN;
    entry.last_time_ticks = INT64_MIN;
    index_offset = encoder.size;
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_write_index(&encoder, &entry, 1u, 0));
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_write_note(&encoder, "hello", 5u, 0));
    TEST_ASSERT_EQUAL_INT(
        SDAF_OK, sdaf_encoder_write_private(&encoder, 0x8001u, NULL, 0u, bytes, sizeof(bytes), 1));
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_encoder_write_end(&encoder, index_offset));
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_decode(encoder.data, encoder.size, NULL, &document));
    TEST_ASSERT_EQUAL_size_t(6u, document.record_count);
    TEST_ASSERT_EQUAL_STRING("warning", document.records[0].value.text.message);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        bytes, document.records[1].value.blob.decoded_payload, sizeof(bytes));
    TEST_ASSERT_EQUAL_STRING("hello", document.records[3].value.note.message);
    TEST_ASSERT_EQUAL_UINT16(0x8001u, document.records[4].envelope.type);
    sdaf_document_free(&document);
    sdaf_encoder_free(&encoder);
}

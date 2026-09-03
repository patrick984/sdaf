#include "sdaf/sdaf.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

static void fixture_path(char* path, size_t size, const char* group, const char* name)
{
    (void)snprintf(path, size, "%s/../../spec/sdaf-conformance/%s/%s", SDAF_SOURCE_DIR, group, name);
}

static void assert_minimal(const char* name, int trailer)
{
    static const uint64_t expected[] = { 0u, 4095u, 291u, 1110u, 2748u, 1929u };
    char path[512];
    sdaf_document document;
    const sdaf_data_record* data;
    size_t sample, value, index = 0u;
    fixture_path(path, sizeof(path), "valid", name);
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_decode_file(path, NULL, &document));
    TEST_ASSERT_EQUAL_size_t(2u, document.record_count);
    data = &document.records[1].value.data;
    TEST_ASSERT_EQUAL_INT(trailer, document.records[1].envelope.payload_crc_in_trailer);
    TEST_ASSERT_NOT_NULL(data->samples);
    for (sample = 0u; sample < data->sample_count; ++sample)
        for (value = 0u; value < data->samples[sample].value_count; ++value)
            TEST_ASSERT_EQUAL_UINT64(
                expected[index++], data->samples[sample].values[value].raw_unsigned);
    sdaf_document_free(&document);
}

void test_valid_minimal_leading(void) { assert_minimal("minimal-leading.sdaf", 0); }
void test_valid_minimal_trailing(void) { assert_minimal("minimal-trailing.sdaf", 1); }

void test_valid_compressed_numeric(void)
{
    static const uint64_t expected[] = { 256u, 512u, 257u, 511u, 255u, 513u };
    char path[512];
    sdaf_document document;
    const sdaf_data_record* data;
    size_t sample, value, index = 0u;
    fixture_path(path, sizeof(path), "valid", "compressed-numeric.sdaf");
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_decode_file(path, NULL, &document));
    data = &document.records[1].value.data;
    TEST_ASSERT_EQUAL_size_t(4u, data->transform_count);
    for (sample = 0u; sample < data->sample_count; ++sample)
        for (value = 0u; value < data->samples[sample].value_count; ++value)
            TEST_ASSERT_EQUAL_UINT64(
                expected[index++], data->samples[sample].values[value].raw_unsigned);
    sdaf_document_free(&document);
}

void test_valid_rational_symbolic(void)
{
    char path[512];
    sdaf_document document;
    const sdaf_sample* sample;
    fixture_path(path, sizeof(path), "valid", "rational-symbolic.sdaf");
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_decode_file(path, NULL, &document));
    sample = &document.records[1].value.data.samples[0];
    TEST_ASSERT_EQUAL_UINT64(49u, sample->values[0].raw_unsigned);
    TEST_ASSERT_EQUAL_INT64(2312, sample->values[1].raw_signed);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 23.12, sample->values[1].physical_value);
    sdaf_document_free(&document);
}

void test_valid_blobs(void)
{
    static const char* names[] = { "blob-uncompressed.sdaf", "blob-zstd.sdaf" };
    static const uint8_t expected[] = { 0xaau, 0x55u, 1u, 2u, 3u };
    size_t i;
    for (i = 0u; i < 2u; ++i) {
        char path[512];
        sdaf_document document;
        fixture_path(path, sizeof(path), "valid", names[i]);
        TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_decode_file(path, NULL, &document));
        TEST_ASSERT_EQUAL_size_t(
            sizeof(expected), document.records[1].value.blob.decoded_payload_size);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(
            expected, document.records[1].value.blob.decoded_payload, sizeof(expected));
        sdaf_document_free(&document);
    }
}

void test_corrupt_fixtures(void)
{
    static const char* names[] = { "bad-file-header-crc.sdaf", "bad-record-header-crc.sdaf",
        "bad-leading-payload-crc.sdaf", "bad-trailer-marker.sdaf", "bad-trailer-sequence.sdaf",
        "malformed-tlv-overrun.sdaf", "invalid-transform-order.sdaf",
        "blob-decoded-size-mismatch.sdaf", "reserved-envelope-flag.sdaf" };
    size_t i;
    for (i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[512];
        sdaf_document document;
        fixture_path(path, sizeof(path), "corrupt", names[i]);
        TEST_ASSERT_NOT_EQUAL(SDAF_OK, sdaf_decode_file(path, NULL, &document));
        sdaf_document_free(&document);
    }
}

void test_truncated_fixtures(void)
{
    char path[512];
    sdaf_document document;
    fixture_path(path, sizeof(path), "corrupt", "truncated-record-header.sdaf");
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_decode_file(path, NULL, &document));
    TEST_ASSERT_EQUAL_size_t(2u, document.record_count);
    sdaf_document_free(&document);
    fixture_path(path, sizeof(path), "corrupt", "truncated-payload.sdaf");
    TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_decode_file(path, NULL, &document));
    TEST_ASSERT_EQUAL_size_t(1u, document.record_count);
    sdaf_document_free(&document);
}

void test_reader_limits_are_distinct_from_malformed_data(void)
{
    char path[512];
    sdaf_document document;
    sdaf_limits limits;
    fixture_path(path, sizeof(path), "valid", "minimal-leading.sdaf");
    sdaf_limits_default(&limits);
    limits.max_payload_size = 8u;
    TEST_ASSERT_EQUAL_INT(SDAF_ERROR_LIMIT, sdaf_decode_file(path, &limits, &document));
    TEST_ASSERT_EQUAL_size_t(0u, document.record_count);
    sdaf_document_free(&document);
}

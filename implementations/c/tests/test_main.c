#include "unity.h"

void test_crc_empty(void);
void test_crc_check_vector(void);
void test_valid_minimal_leading(void);
void test_valid_minimal_trailing(void);
void test_valid_compressed_numeric(void);
void test_valid_rational_symbolic(void);
void test_valid_blobs(void);
void test_corrupt_fixtures(void);
void test_truncated_fixtures(void);
void test_reader_limits_are_distinct_from_malformed_data(void);
void test_roundtrip_identity(void);
void test_roundtrip_trailer(void);
void test_roundtrip_zstandard(void);
void test_roundtrip_numeric_profile(void);
void test_roundtrip_bitwise_f32(void);
void test_roundtrip_bitwise_f64(void);
void test_typed_profiles_reject_wrong_channel_kind(void);
void test_roundtrip_text_blob_index_end_private(void);
void test_byte_aligned_mixed_types_and_wide_fixed_bytes(void);
void test_planar_layout_reassembles_samples(void);
void test_delta_timestamps(void);
void test_explicit_timestamps(void);
void test_dense_padding_is_validated(void);
void test_cli_json(void);
void test_cli_csv(void);
void test_cli_cbor(void);

void setUp(void) { }
void tearDown(void) { }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_empty);
    RUN_TEST(test_crc_check_vector);
    RUN_TEST(test_valid_minimal_leading);
    RUN_TEST(test_valid_minimal_trailing);
    RUN_TEST(test_valid_compressed_numeric);
    RUN_TEST(test_valid_rational_symbolic);
    RUN_TEST(test_valid_blobs);
    RUN_TEST(test_corrupt_fixtures);
    RUN_TEST(test_truncated_fixtures);
    RUN_TEST(test_reader_limits_are_distinct_from_malformed_data);
    RUN_TEST(test_roundtrip_identity);
    RUN_TEST(test_roundtrip_trailer);
    RUN_TEST(test_roundtrip_zstandard);
    RUN_TEST(test_roundtrip_numeric_profile);
    RUN_TEST(test_roundtrip_bitwise_f32);
    RUN_TEST(test_roundtrip_bitwise_f64);
    RUN_TEST(test_typed_profiles_reject_wrong_channel_kind);
    RUN_TEST(test_roundtrip_text_blob_index_end_private);
    RUN_TEST(test_byte_aligned_mixed_types_and_wide_fixed_bytes);
    RUN_TEST(test_planar_layout_reassembles_samples);
    RUN_TEST(test_delta_timestamps);
    RUN_TEST(test_explicit_timestamps);
    RUN_TEST(test_dense_padding_is_validated);
    RUN_TEST(test_cli_json);
    RUN_TEST(test_cli_csv);
    RUN_TEST(test_cli_cbor);
    return UNITY_END();
}

#include "exports.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

static void load_fixture(sdaf_document *document)
{
    char path[512]; (void)snprintf(path, sizeof(path), "%s/sdaf-conformance/valid/minimal-leading.sdaf", SDAF_SOURCE_DIR); TEST_ASSERT_EQUAL_INT(SDAF_OK, sdaf_decode_file(path, NULL, document));
}

static size_t read_tmp(FILE *file, uint8_t *buffer, size_t capacity)
{
    long length; TEST_ASSERT_EQUAL_INT(0, fflush(file)); TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END)); length = ftell(file); TEST_ASSERT_GREATER_OR_EQUAL_INT(0, length); TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET)); TEST_ASSERT_TRUE((size_t)length < capacity); return fread(buffer, 1u, (size_t)length, file);
}

void test_cli_json(void)
{
    sdaf_document document; FILE *file = tmpfile(); uint8_t output[8192]; size_t size;
    TEST_ASSERT_NOT_NULL(file); load_fixture(&document); TEST_ASSERT_EQUAL_INT(0, sdaf_cli_write_json(file, &document)); size = read_tmp(file, output, sizeof(output)); output[size] = 0u; TEST_ASSERT_NOT_NULL(strstr((char *)output, "\"type\":\"DATA\"")); TEST_ASSERT_NOT_NULL(strstr((char *)output, "\"raw\":4095")); fclose(file); sdaf_document_free(&document);
}

void test_cli_csv(void)
{
    sdaf_document document; FILE *file = tmpfile(); uint8_t output[4096]; size_t size;
    TEST_ASSERT_NOT_NULL(file); load_fixture(&document); TEST_ASSERT_EQUAL_INT(0, sdaf_cli_write_csv(file, &document)); size = read_tmp(file, output, sizeof(output)); output[size] = 0u; TEST_ASSERT_NOT_NULL(strstr((char *)output, "1,1,2,1000002,2,adc1,0,1929,1929,code")); fclose(file); sdaf_document_free(&document);
}

void test_cli_cbor(void)
{
    sdaf_document document; FILE *file = tmpfile(); uint8_t output[1024]; size_t size;
    TEST_ASSERT_NOT_NULL(file); load_fixture(&document); TEST_ASSERT_EQUAL_INT(0, sdaf_cli_write_cbor(file, &document)); size = read_tmp(file, output, sizeof(output)); TEST_ASSERT_GREATER_THAN(10u, size); TEST_ASSERT_EQUAL_HEX8(0xa2u, output[0]); fclose(file); sdaf_document_free(&document);
}

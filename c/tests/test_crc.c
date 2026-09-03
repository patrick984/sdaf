#include "sdaf/sdaf.h"
#include "unity.h"

#include <string.h>

void test_crc_empty(void) { TEST_ASSERT_EQUAL_UINT32(0u, sdaf_crc32c("", 0u)); }

void test_crc_check_vector(void)
{
    const char* vector = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xe3069283u, sdaf_crc32c(vector, strlen(vector)));
}

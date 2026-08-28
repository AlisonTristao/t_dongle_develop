#include <ClockText.h>
#include <unity.h>

#include <ctime>

void setUp() {}
void tearDown() {}

void test_unix_epoch_form_is_exact() {
    time_t epoch = 0;
    TEST_ASSERT_TRUE(ClockText::parse("@1787882561", epoch));
    TEST_ASSERT_EQUAL_INT64(1787882561LL, static_cast<long long>(epoch));
}

void test_unix_epoch_form_rejects_partial_and_nonpositive_values() {
    time_t epoch = 123;
    TEST_ASSERT_FALSE(ClockText::parse("@1787882561junk", epoch));
    TEST_ASSERT_FALSE(ClockText::parse("@0", epoch));
    TEST_ASSERT_FALSE(ClockText::parse("@-1", epoch));
    TEST_ASSERT_FALSE(ClockText::parse("@", epoch));
}

void test_human_calendar_form_remains_supported() {
    time_t epoch = 0;
    TEST_ASSERT_TRUE(ClockText::parse("2026-08-27 23:02:41", epoch));
    TEST_ASSERT_TRUE(epoch > 0);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_unix_epoch_form_is_exact);
    RUN_TEST(test_unix_epoch_form_rejects_partial_and_nonpositive_values);
    RUN_TEST(test_human_calendar_form_remains_supported);
    return UNITY_END();
}

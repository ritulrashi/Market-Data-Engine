#include "test_framework.hpp"

void test_wire_layout();
void test_single_threaded_order();
void test_drop_detection();
void test_multi_consumer_concurrent();

int main() {
    RUN_TEST(test_wire_layout);
    RUN_TEST(test_single_threaded_order);
    RUN_TEST(test_drop_detection);
    RUN_TEST(test_multi_consumer_concurrent);

    if (g_test_failures == 0) {
        std::cerr << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cerr << g_test_failures << " CHECK(S) FAILED\n";
    return 1;
}

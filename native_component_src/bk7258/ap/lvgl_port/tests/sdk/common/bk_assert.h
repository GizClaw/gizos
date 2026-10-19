#ifndef H2_BK_DMA2D_TEST_ASSERT_H
#define H2_BK_DMA2D_TEST_ASSERT_H
void test_sdk_assert(void);
#define BK_ASSERT(expression) do { if (!(expression)) test_sdk_assert(); } while (0)
#endif

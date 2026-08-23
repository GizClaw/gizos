#include <stdio.h>

int h2_toolchain_cross_symbol(void)
{
    return 42;
}

int main(void)
{
    return printf("%d\n", h2_toolchain_cross_symbol()) < 0 ? 1 : 0;
}

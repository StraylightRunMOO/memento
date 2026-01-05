#include <stdio.h>
#include <stdint.h>

int main() {
    printf("SIZE_MAX = %zu\n", SIZE_MAX);
    printf("SIZE_MAX / 1024 = %zu KB\n", SIZE_MAX / 1024);
    printf("SIZE_MAX / (1024*1024) = %zu MB\n", SIZE_MAX / (1024*1024));
    printf("SIZE_MAX / (1024*1024*1024) = %zu GB\n", SIZE_MAX / (1024*1024*1024));
    return 0;
}
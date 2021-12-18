#include <iostream>
#include <vector>
#include "heap.hpp"

int main()
{
    printf("Entering main()\n");

    int* test1 = (int*)my_malloc(400);
    *test1 = 10;
    my_heap_dump();

    int* test2 = (int*)my_malloc(4);
    *test2 = 15;
    my_heap_dump();

    std::cout << *test1 << ", " << *test2 << std::endl;

    my_free(test1);
    my_heap_dump();

    short* test3 = (short*)my_malloc(4); // Should be equal to test1
    *test3 = 0x0001;
    my_heap_dump();

    short* test4 = (short*)my_malloc(2); // Should be equal to test1
    *test4 = 0x0000;
    my_heap_dump();

    std::cout << *test3 << ", " << *test2 << std::endl;

    // maybe test blocks that don't fit??
    int* testtest = (int*)my_malloc(500);
    *testtest = 0x12345678;
    my_heap_dump();

    // and big blocks
    int* testbig = (int*)my_malloc(100000);
    *testbig = 2137;
    std::cout << "testbig=" << *testbig << std::endl;
    my_free(testbig);

    // Some real example
    std::cout << "----TEST----" << std::endl;
    int* pointers[1000];
    for(size_t s = 0; s < 1000; s++)
    {
        pointers[s] = (int*)my_malloc(256);
        *pointers[s] = rand() % 10;
    }
    for(size_t s = 0; s < 1000; s++)
    {
        std::cout << *pointers[s] << ",";
        my_free(pointers[s]);
    }
    std::cout << "----TEST END----" << std::endl;

    // Double-free at end :)
    // (This will probably not crash because this block is reused)
    std::cout << "End Main" << std::endl;
    my_heap_dump();
    my_free(test1);
    my_leak_check();
    return 0;
}

#include <cstdio>
#include "include/cthreadpool.h"
#include <string>

class class_handle {
public:
	void operator()(size_t n, const std::string& name) {
		printf("class_handle: %zu, %s\n", n, name.c_str());
	}
};

class class_handle2 {
public:
	void operator()(size_t n, size_t n1, const std::string& name) {
		printf("class_handle2: %zu,%zu, %s\n", n, n1, name.c_str());
	}
};


int main()
{
    printf("hello from threadpool!\n");

	cthreadpool pool(5,10);

	pool.enqueue<class_handle>(10,"Hello");

	pool.enqueue<class_handle2>(12,90, "Hello2");

	pool.enqueue([](size_t n, size_t n1, size_t n2, const std::string& name) {
		printf("function: %zu,%zu,%zu, %s\n", n, n1, n2, name.c_str());
	},14,95,8,"Hello3");

	pool.join();

    return 0;
}
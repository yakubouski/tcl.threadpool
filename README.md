# Template class library module
C++ threadpool template class library

#### threadpool example
```c++

class class_handle {
public:
	/* operator () overload */
	void operator()(size_t n, const std::string& name) {
		printf("class_handle: %zu, %s\n", n, name.c_str());
	}
};

class class_handle2 {
public:
	/* operator () overload */
	void operator()(size_t n, size_t n1, const std::string& name) {
		printf("class_handle2: %zu,%zu, %s\n", n, n1, name.c_str());
	}
};


int main()
{
	/* Create threadpool with five persistent workers*/
    tcl::threadpool pool(5,10);

	/* Enqueue job with class handle  */
	pool.enqueue<class_handle>(10,"Hello");

	/* Enqueue job with other class handle  */
	pool.enqueue<class_handle2>(12,90, "Hello2");

	/* Enqueue job with lambda or function  */
	pool.enqueue([](size_t n, size_t n1, size_t n2, const std::string& name) {
		printf("function: %zu,%zu,%zu, %s\n", n, n1, n2, name.c_str());
	},14,95,8,"Hello3");

	/* Wait until all jobs processed */
	pool.join();

    return 0;
}

```

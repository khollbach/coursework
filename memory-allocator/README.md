This is a parallel memory allocator, written for an advanced operating systems course. The API is the same as libc malloc:
- `void *malloc(size_t size);` requests a chunk of contiguous memory, and
- `void free(void *ptr);` releases a chunk that's no longer in use.

The design is based on [this paper](https://www.cs.utexas.edu/users/mckinley/papers/asplos-2000.pdf), which is quite nice. The authors balance a bunch of seemingly at-odds tradeoffs:
- **Sequential speed**: if you're only calling this from one thread, the performance shouldn't suck.
- **Scalability**: the more threads using this concurrently, the higher the *throughput* -- the number of mallocs and frees per second. The most naive implementation would just acquire a global lock during each call to `malloc` or `free`, but this wouldn't scale at all: only the thread holding the lock would make progress, while everyone else is blocked waiting.
- **Avoid false sharing** of cache lines. Suppose you have two threads running on different CPUs, and each thread calls `malloc` to get a few bytes of memory for its own use. If these allocations live on the same cache line, then both threads are slowed down dramatically when using that memory: the underlying hardware spends all this time sychronizing the view of that cache line between the two processors, even though they never intended to share memory.
- **Bound memory overhead** by a constant factor. In particular, don't use a disjoint pool of memory for each thread. Memory freed by one thread should be a candidate for use by another thread. The paper goes into more detail on this, and they give a compelling case for having such a bound. This requirement adds a lot of complexity to the implementation and analysis, but it ties the paper together quite nicely as a mix of theory and practice.

This assignment was graded according to how well our code performed on the benchmarks. My partner and I won! Our implementation performed the best, and we each got a chocolate bar as a prize.

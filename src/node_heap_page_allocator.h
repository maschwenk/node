#ifndef SRC_NODE_HEAP_PAGE_ALLOCATOR_H_
#define SRC_NODE_HEAP_PAGE_ALLOCATOR_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "v8-platform.h"

namespace node {

// Returns the page allocator that Node hands to V8 through its platform, or
// nullptr to let V8 use its built-in allocator (the default). It is chosen
// once per process from the NODE_HEAP_HUGEPAGES environment variable; see
// node_heap_page_allocator.cc. The returned object lives until the process
// exits and is safe to use from any thread.
v8::PageAllocator* GetHeapPageAllocatorFromEnvironment();

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_HEAP_PAGE_ALLOCATOR_H_

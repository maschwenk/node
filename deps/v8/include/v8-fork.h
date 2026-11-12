// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef INCLUDE_V8_FORK_H_
#define INCLUDE_V8_FORK_H_

#include "v8config.h"  // NOLINT(build/include_directory)

namespace v8 {

class Isolate;

/**
 * Experimental, for embedders that fork(2) a process with a live isolate.
 *
 * Reseeds the isolate's random number generator from fresh entropy and resets
 * the Math.random() state of every native context of the isolate, so that the
 * next Math.random() call in each context starts a new sequence. Without this,
 * a forked child produces the same Math.random() sequences as its parent and
 * its siblings. Does nothing when --random-seed is set (the embedder asked for
 * reproducible sequences). Must be called on the isolate's thread.
 */
V8_EXPORT void ReseedRandomNumberGeneratorsAfterFork(Isolate* isolate);

/**
 * Experimental, for embedders that fork(2) a process with a live isolate.
 *
 * Built with v8_enable_private_mapping_fork_optimization (the Linux default in
 * Node.js), V8 marks the memory it maps MADV_DONTFORK, so that fork+exec does
 * not copy the page tables of the heap; a forked child then has no heap.
 * MakeMappingsForkable() clears that mark on every mapping V8 holds, and new
 * mappings are not marked until MakeMappingsUnforkable() marks them all
 * again; call that before a fork that is followed by exec. Each call that
 * changes the state costs one madvise() per mapping; calls that do not
 * change it are cheap. The state is inherited by forked children.
 * Thread-safe. Do nothing where the optimization is not built in.
 */
V8_EXPORT void MakeMappingsForkable();
V8_EXPORT void MakeMappingsUnforkable();

/**
 * Experimental, for embedders that fork(2) a process with a live isolate.
 *
 * With --perf-basic-prof (or --perf-basic-prof-only-functions), call this in
 * the child: it copies the parent's perf-<pid>.map, which describes the code
 * the child inherited, to the child's own perf-<pid>.map and writes new
 * entries there. Does nothing otherwise. --perf-prof (jitdump) is not handled.
 */
V8_EXPORT void ReopenPerfMapAfterFork();

}  // namespace v8

#endif  // INCLUDE_V8_FORK_H_

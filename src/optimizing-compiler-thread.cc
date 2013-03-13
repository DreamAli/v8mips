// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "optimizing-compiler-thread.h"

#include "v8.h"

#include "hydrogen.h"
#include "isolate.h"
#include "v8threads.h"

namespace v8 {
namespace internal {


void OptimizingCompilerThread::Run() {
#ifdef DEBUG
  thread_id_ = ThreadId::Current().ToInteger();
#endif
  Isolate::SetIsolateThreadLocals(isolate_, NULL);

  int64_t epoch = 0;
  if (FLAG_trace_parallel_recompilation) epoch = OS::Ticks();

  while (true) {
    input_queue_semaphore_->Wait();
    Logger::TimerEventScope timer(
        isolate_, Logger::TimerEventScope::v8_recompile_parallel);

    if (FLAG_parallel_recompilation_delay != 0) {
      OS::Sleep(FLAG_parallel_recompilation_delay);
    }

    if (Acquire_Load(&stop_thread_)) {
      stop_semaphore_->Signal();
      if (FLAG_trace_parallel_recompilation) {
        time_spent_total_ = OS::Ticks() - epoch;
      }
      return;
    }

    int64_t compiling_start = 0;
    if (FLAG_trace_parallel_recompilation) compiling_start = OS::Ticks();

    CompileNext();

    if (FLAG_trace_parallel_recompilation) {
      time_spent_compiling_ += OS::Ticks() - compiling_start;
    }
  }
}


void OptimizingCompilerThread::CompileNext() {
  Heap::RelocationLock relocation_lock(isolate_->heap());
  OptimizingCompiler* optimizing_compiler = NULL;
  input_queue_.Dequeue(&optimizing_compiler);
  Barrier_AtomicIncrement(&queue_length_, static_cast<Atomic32>(-1));

  ASSERT(optimizing_compiler->info()->closure()->IsInRecompileQueue());

  OptimizingCompiler::Status status = optimizing_compiler->OptimizeGraph();
  ASSERT(status != OptimizingCompiler::FAILED);
  // Prevent an unused-variable error in release mode.
  USE(status);

  output_queue_.Enqueue(optimizing_compiler);

  // The execution thread can call InstallOptimizedFunctions() at any time,
  // including at this point, after queuing for install and before marking
  // for install.  To avoid race condition, functions that are queued but not
  // yet marked for install are not processed by InstallOptimizedFunctions().

  ASSERT(optimizing_compiler->info()->closure()->IsInRecompileQueue());
  // Mark function to generate and install optimized code.  We assume this
  // write to be atomic.
  optimizing_compiler->info()->closure()->MarkForInstallingRecompiledCode();
}


void OptimizingCompilerThread::Stop() {
  ASSERT(!IsOptimizerThread());
  Release_Store(&stop_thread_, static_cast<AtomicWord>(true));
  input_queue_semaphore_->Signal();
  stop_semaphore_->Wait();

  if (FLAG_parallel_recompilation_delay != 0) {
    // Execution ended before we managed to compile and install the remaining
    // functions in the queue.  We still want to do that for debugging though.
    // At this point the optimizing thread already stopped, so we finish
    // processing the queue in the main thread.
    InstallOptimizedFunctions();
    // Barrier when loading queue length is not necessary since the write
    // happens in CompileNext on the same thread.
    while (NoBarrier_Load(&queue_length_) > 0) {
      CompileNext();
      InstallOptimizedFunctions();
    }
  }

  if (FLAG_trace_parallel_recompilation) {
    double compile_time = static_cast<double>(time_spent_compiling_);
    double total_time = static_cast<double>(time_spent_total_);
    double percentage = (compile_time * 100) / total_time;
    PrintF("  ** Compiler thread did %.2f%% useful work\n", percentage);
  }
}


void OptimizingCompilerThread::InstallOptimizedFunctions() {
  ASSERT(!IsOptimizerThread());
  HandleScope handle_scope(isolate_);
  int functions_installed = 0;
  while (!output_queue_.IsEmpty()) {
    OptimizingCompiler* compiler = *output_queue_.Peek();

    if (compiler->info()->closure()->IsInRecompileQueue()) {
      // A function may be queued for install, but not marked as such yet.
      // We continue with the output queue the next to avoid race condition.
      break;
    }
    output_queue_.Dequeue(&compiler);

#ifdef DEBUG
    // Create new closure handle since the deferred handle is about to die.
    Handle<JSFunction> closure(*compiler->info()->closure());
#endif  // DEBUG

    Compiler::InstallOptimizedCode(compiler);
    // Assert that the marker builtin has been replaced by actual code.
    ASSERT(!closure->IsInRecompileQueue());
    functions_installed++;
  }
}


void OptimizingCompilerThread::QueueForOptimization(
    OptimizingCompiler* optimizing_compiler) {
  ASSERT(IsQueueAvailable());
  ASSERT(!IsOptimizerThread());
  Barrier_AtomicIncrement(&queue_length_, static_cast<Atomic32>(1));
  input_queue_.Enqueue(optimizing_compiler);
  input_queue_semaphore_->Signal();
}


#ifdef DEBUG
bool OptimizingCompilerThread::IsOptimizerThread() {
  if (!FLAG_parallel_recompilation) return false;
  return ThreadId::Current().ToInteger() == thread_id_;
}
#endif


} }  // namespace v8::internal

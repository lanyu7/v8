// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/embedder-tracing.h"

#include "src/base/logging.h"
#include "src/heap/gc-tracer.h"
#include "src/objects/embedder-data-slot.h"
#include "src/objects/js-objects-inl.h"

namespace v8 {
namespace internal {

void LocalEmbedderHeapTracer::SetRemoteTracer(EmbedderHeapTracer* tracer) {
  if (remote_tracer_) remote_tracer_->isolate_ = nullptr;

  remote_tracer_ = tracer;
  if (remote_tracer_)
    remote_tracer_->isolate_ = reinterpret_cast<v8::Isolate*>(isolate_);
}

void LocalEmbedderHeapTracer::TracePrologue(
    EmbedderHeapTracer::TraceFlags flags) {
  if (!InUse()) return;

  embedder_worklist_empty_ = false;
  remote_tracer_->TracePrologue(flags);
}

void LocalEmbedderHeapTracer::TraceEpilogue() {
  if (!InUse()) return;

  EmbedderHeapTracer::TraceSummary summary;
  remote_tracer_->TraceEpilogue(&summary);
  remote_stats_.used_size = summary.allocated_size;
  // Force a check next time increased memory is reported. This allows for
  // setting limits close to actual heap sizes.
  remote_stats_.allocated_size_limit_for_check = 0;
  constexpr double kMinReportingTimeMs = 0.5;
  if (summary.time > kMinReportingTimeMs) {
    isolate_->heap()->tracer()->RecordEmbedderSpeed(summary.allocated_size,
                                                    summary.time);
  }
}

void LocalEmbedderHeapTracer::EnterFinalPause() {
  if (!InUse()) return;

  remote_tracer_->EnterFinalPause(embedder_stack_state_);
  // Resetting to state unknown as there may be follow up garbage collections
  // triggered from callbacks that have a different stack state.
  embedder_stack_state_ =
      EmbedderHeapTracer::EmbedderStackState::kMayContainHeapPointers;
}

bool LocalEmbedderHeapTracer::Trace(double deadline) {
  if (!InUse()) return true;

  return remote_tracer_->AdvanceTracing(deadline);
}

bool LocalEmbedderHeapTracer::IsRemoteTracingDone() {
  return !InUse() || remote_tracer_->IsTracingDone();
}

void LocalEmbedderHeapTracer::SetEmbedderStackStateForNextFinalization(
    EmbedderHeapTracer::EmbedderStackState stack_state) {
  if (!InUse()) return;

  embedder_stack_state_ = stack_state;
  if (EmbedderHeapTracer::EmbedderStackState::kNoHeapPointers == stack_state) {
    remote_tracer()->NotifyEmptyEmbedderStack();
  }
}

LocalEmbedderHeapTracer::ProcessingScope::ProcessingScope(
    LocalEmbedderHeapTracer* tracer)
    : tracer_(tracer) {
  wrapper_cache_.reserve(kWrapperCacheSize);
}

LocalEmbedderHeapTracer::ProcessingScope::~ProcessingScope() {
  if (!wrapper_cache_.empty()) {
    tracer_->remote_tracer()->RegisterV8References(std::move(wrapper_cache_));
  }
}

// static
LocalEmbedderHeapTracer::WrapperInfo
LocalEmbedderHeapTracer::ExtractWrapperInfo(Isolate* isolate,
                                            JSObject js_object) {
  DCHECK_GE(js_object.GetEmbedderFieldCount(), 2);
  DCHECK(js_object.IsApiWrapper());

  WrapperInfo info;
  if (EmbedderDataSlot(js_object, 0)
          .ToAlignedPointerSafe(isolate, &info.first) &&
      info.first &&
      EmbedderDataSlot(js_object, 1)
          .ToAlignedPointerSafe(isolate, &info.second)) {
    return info;
  }
  return {nullptr, nullptr};
}

void LocalEmbedderHeapTracer::ProcessingScope::TracePossibleWrapper(
    JSObject js_object) {
  DCHECK(js_object.IsApiWrapper());
  if (js_object.GetEmbedderFieldCount() < 2) return;

  WrapperInfo info =
      LocalEmbedderHeapTracer::ExtractWrapperInfo(tracer_->isolate_, js_object);
  if (VerboseWrapperInfo(info).is_valid()) {
    wrapper_cache_.push_back(std::move(info));
  }
  FlushWrapperCacheIfFull();
}

void LocalEmbedderHeapTracer::ProcessingScope::FlushWrapperCacheIfFull() {
  if (wrapper_cache_.size() == wrapper_cache_.capacity()) {
    tracer_->remote_tracer()->RegisterV8References(std::move(wrapper_cache_));
    wrapper_cache_.clear();
    wrapper_cache_.reserve(kWrapperCacheSize);
  }
}

void LocalEmbedderHeapTracer::ProcessingScope::AddWrapperInfoForTesting(
    WrapperInfo info) {
  wrapper_cache_.push_back(info);
  FlushWrapperCacheIfFull();
}

void LocalEmbedderHeapTracer::StartIncrementalMarkingIfNeeded() {
  if (!FLAG_global_gc_scheduling || !FLAG_incremental_marking) return;

  Heap* heap = isolate_->heap();
  heap->StartIncrementalMarkingIfAllocationLimitIsReached(
      heap->GCFlagsForIncrementalMarking(),
      kGCCallbackScheduleIdleGarbageCollection);
  if (heap->AllocationLimitOvershotByLargeMargin()) {
    heap->FinalizeIncrementalMarkingAtomically(
        i::GarbageCollectionReason::kExternalFinalize);
  }
}

}  // namespace internal
}  // namespace v8

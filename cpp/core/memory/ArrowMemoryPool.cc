/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ArrowMemoryPool.h"
#include "utils/exception.h"

#include <sys/mman.h>

namespace gluten {

std::shared_ptr<arrow::MemoryPool> asWrappedArrowMemoryPool(MemoryAllocator* allocator) {
  return std::make_shared<WrappedArrowMemoryPool>(allocator);
}

std::shared_ptr<arrow::MemoryPool> getDefaultArrowMemoryPool() {
  static auto staticPool = asWrappedArrowMemoryPool(defaultMemoryAllocator().get());
  return staticPool;
}

arrow::Status WrappedArrowMemoryPool::Allocate(int64_t size, int64_t alignment, uint8_t** out) {
  if (!allocator_->allocateAligned(alignment, size, reinterpret_cast<void**>(out))) {
    return arrow::Status::Invalid("WrappedMemoryPool: Error allocating " + std::to_string(size) + " bytes");
  }
  madvise(*out, size, MADV_WILLNEED);
  return arrow::Status::OK();
}

arrow::Status WrappedArrowMemoryPool::Reallocate(int64_t oldSize, int64_t newSize, int64_t alignment, uint8_t** ptr) {
  if (!allocator_->reallocateAligned(*ptr, alignment, oldSize, newSize, reinterpret_cast<void**>(ptr))) {
    return arrow::Status::Invalid("WrappedMemoryPool: Error reallocating " + std::to_string(newSize) + " bytes");
  }
  madvise(*ptr, newSize, MADV_WILLNEED);
  return arrow::Status::OK();
}

void WrappedArrowMemoryPool::Free(uint8_t* buffer, int64_t size, int64_t alignment) {
  allocator_->free(buffer, size);
}

int64_t WrappedArrowMemoryPool::bytes_allocated() const {
  // fixme use self accountant
  return allocator_->getBytes();
}

int64_t WrappedArrowMemoryPool::total_bytes_allocated() const {
  throw GlutenException("Not implement");
}

int64_t WrappedArrowMemoryPool::num_allocations() const {
  throw GlutenException("Not implement");
}

std::string WrappedArrowMemoryPool::backend_name() const {
  return "gluten allocator";
}

} // namespace gluten

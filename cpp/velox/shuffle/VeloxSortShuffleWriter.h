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

#pragma once

#include "shuffle/VeloxShuffleWriter.h"

#include <arrow/io/buffered.h>
#include <arrow/result.h>

namespace gluten {

enum SortState { kSortInit, kSort, kSortStop };

class VeloxSortShuffleWriter final : public VeloxShuffleWriter {
 public:
  static arrow::Result<std::shared_ptr<VeloxShuffleWriter>> create(
      uint32_t numPartitions,
      std::unique_ptr<PartitionWriter> partitionWriter,
      ShuffleWriterOptions options,
      std::shared_ptr<facebook::velox::memory::MemoryPool> veloxPool,
      arrow::MemoryPool* arrowPool);

  arrow::Status write(std::shared_ptr<ColumnarBatch> cb, int64_t memLimit) override;

  arrow::Status stop() override;

  arrow::Status reclaimFixedSize(int64_t size, int64_t* actual) override;

  arrow::Status evictRowVector(uint32_t partitionId) override;

 private:
  VeloxSortShuffleWriter(
      uint32_t numPartitions,
      std::unique_ptr<PartitionWriter> partitionWriter,
      ShuffleWriterOptions options,
      std::shared_ptr<facebook::velox::memory::MemoryPool> veloxPool,
      arrow::MemoryPool* pool)
      : VeloxShuffleWriter(numPartitions, std::move(partitionWriter), std::move(options), std::move(veloxPool), pool) {}

  arrow::Status init();

  arrow::Status initFromRowVector(const facebook::velox::RowVector& rv);

  void setSortState(SortState state);

  arrow::Status doSort(facebook::velox::RowVectorPtr rv, int64_t /* memLimit */);

  arrow::Status evictBatch(uint32_t partitionId);

  arrow::Status partitioningAndEvict(facebook::velox::RowVectorPtr rv, int64_t memLimit);

  arrow::Status localSort(
      const facebook::velox::VectorPtr& vector,
      const std::vector<uint32_t>& row2Partition,
      facebook::velox::memory::MemoryPool* pool);

  // Row ID -> Partition ID
  // subscript: The index of row in the current input RowVector
  // value: Partition ID
  // Updated for each input RowVector.
  std::vector<uint32_t> row2Partition_;

  // Partition ID -> Row Count
  // subscript: Partition ID
  // value: How many rows does this partition have in the current input RowVector
  // Updated for each input RowVector.
  std::vector<uint32_t> partition2RowCount_;

  // Partition ID -> RowVector + RowRange
  // subscript: Partition ID
  std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, folly::Range<uint32_t>>>> partition2RowVector_;

  facebook::velox::RowTypePtr rowType_;

  std::unique_ptr<facebook::velox::VectorStreamGroup> batch_;
  std::unique_ptr<arrow::io::BufferOutputStream> bufferOutputStream_;

  std::unique_ptr<facebook::velox::serializer::presto::PrestoVectorSerde> serde_ =
      std::make_unique<facebook::velox::serializer::presto::PrestoVectorSerde>();

  std::vector<facebook::velox::RowVectorPtr> batches_;

  std::unordered_map<int32_t, std::vector<int64_t>> rowVectorIndexMap_;

  uint32_t currentInputColumnBytes_ = 0;

  SortState sortState_{kSortInit};
}; // class VeloxSortBasedShuffleWriter

} // namespace gluten

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

#include <arrow/status.h>
#include <map>
#include <vector>

#include "shuffle/VeloxShuffleWriter.h"
#include "velox/row/CompactRow.h"
#include "velox/vector/BaseVector.h"

namespace gluten {

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

 private:
  VeloxSortShuffleWriter(
      uint32_t numPartitions,
      std::unique_ptr<PartitionWriter> partitionWriter,
      ShuffleWriterOptions options,
      std::shared_ptr<facebook::velox::memory::MemoryPool> veloxPool,
      arrow::MemoryPool* pool);

  void init();

  void initRowType(const facebook::velox::RowVectorPtr& rv);

  arrow::Result<facebook::velox::RowVectorPtr> getPeeledRowVector(const std::shared_ptr<ColumnarBatch>& cb);

  arrow::Status insert(const facebook::velox::RowVectorPtr& vector, int64_t memLimit);

  arrow::Status evictAllPartitions();

  arrow::Status evictPartition(uint32_t partitionId, size_t begin, size_t end);

  arrow::Status spillIfNeeded(int64_t memLimit);

  void acquireNewBuffer(int64_t memLimit, uint64_t minSizeRequired);

  uint32_t maxRowsToInsert(uint32_t offset, uint32_t rows);

  void insertRows(facebook::velox::row::CompactRow& row, uint32_t offset, uint32_t rows);

  uint16_t numInputs_{0};
  uint64_t writeOffset_{0};
  uint64_t totalRows_{0};
  // Stores inputs in row format.
  facebook::velox::BufferPtr rowBuffer_;
  std::list<facebook::velox::BufferPtr> cachedInputBuffer_;
  // Stores compact row id -> row
  std::vector<std::pair<uint64_t, std::string_view>> data_;

  facebook::velox::BufferPtr sortedBuffer_;

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

  std::shared_ptr<const facebook::velox::RowType> rowType_;
  std::optional<int32_t> fixedRowSize_;
  std::vector<uint32_t> rowSizes_;
};
} // namespace gluten

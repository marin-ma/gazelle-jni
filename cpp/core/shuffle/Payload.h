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

#include <arrow/buffer.h>
#include <arrow/io/interfaces.h>
#include <arrow/memory_pool.h>

#include "shuffle/Options.h"

namespace gluten {

class Payload {
 public:
  enum Type : int32_t { kCompressed, kUncompressed, kToBeCompressed };

  Payload(Type type, uint32_t numRows, const std::vector<bool>* isValidityBuffer)
      : type_(type), numRows_(numRows), isValidityBuffer_(isValidityBuffer) {}

  virtual ~Payload() = default;

  virtual arrow::Status serialize(arrow::io::OutputStream* outputStream) = 0;

  virtual arrow::Result<std::shared_ptr<arrow::Buffer>> readBufferAt(uint32_t index) = 0;

  int64_t getCompressTime() const {
    return compressTime_;
  }

  int64_t getWriteTime() const {
    return writeTime_;
  }

  Type type() const {
    return type_;
  }

  uint32_t numRows() const {
    return numRows_;
  }

  uint32_t numBuffers() {
    return isValidityBuffer_->size();
  }

  const std::vector<bool>* isValidityBuffer() const {
    return isValidityBuffer_;
  }

  std::string toString() const {
    static std::string kUncompressedString = "Payload::kUncompressed";
    static std::string kCompressedString = "Payload::kCompressed";
    static std::string kToBeCompressedString = "Payload::kToBeCompressed";

    if (type_ == kUncompressed) {
      return kUncompressedString;
    }
    if (type_ == kCompressed) {
      return kCompressedString;
    }
    return kToBeCompressedString;
  }

 protected:
  Type type_;
  uint32_t numRows_;
  const std::vector<bool>* isValidityBuffer_;
  int64_t compressTime_{0};
  int64_t writeTime_{0};
};

// A block represents data to be cached in-memory.
// Can be compressed or uncompressed.
class BlockPayload : public Payload {
 public:
  static arrow::Result<std::unique_ptr<BlockPayload>> fromBuffers(
      Payload::Type payloadType,
      uint32_t numRows,
      std::vector<std::shared_ptr<arrow::Buffer>> buffers,
      const std::vector<bool>* isValidityBuffer,
      arrow::MemoryPool* pool,
      arrow::util::Codec* codec);

  static arrow::Result<std::vector<std::shared_ptr<arrow::Buffer>>> deserialize(
      arrow::io::InputStream* inputStream,
      const std::shared_ptr<arrow::Schema>& schema,
      const std::shared_ptr<arrow::util::Codec>& codec,
      arrow::MemoryPool* pool,
      uint32_t& numRows);

  static arrow::Result<std::shared_ptr<arrow::Buffer>> readUncompressedBuffer(arrow::io::InputStream* inputStream);

  static arrow::Result<std::shared_ptr<arrow::Buffer>> readCompressedBuffer(
      arrow::io::InputStream* inputStream,
      const std::shared_ptr<arrow::util::Codec>& codec,
      arrow::MemoryPool* pool);

  arrow::Status serialize(arrow::io::OutputStream* outputStream) override;

  arrow::Result<std::shared_ptr<arrow::Buffer>> readBufferAt(uint32_t pos) override;

 protected:
  BlockPayload(
      Type type,
      uint32_t numRows,
      std::vector<std::shared_ptr<arrow::Buffer>> buffers,
      const std::vector<bool>* isValidityBuffer,
      arrow::MemoryPool* pool,
      arrow::util::Codec* codec)
      : Payload(type, numRows, isValidityBuffer), buffers_(std::move(buffers)), pool_(pool), codec_(codec) {}

  void setCompressionTime(int64_t compressionTime) {
    compressTime_ = compressionTime;
  }

  std::vector<std::shared_ptr<arrow::Buffer>> buffers_;
  arrow::MemoryPool* pool_;
  arrow::util::Codec* codec_;
};

// Type of MergeBlockPayload can be either kMergedCompressed or kMergedUncompressed.
class MergeBlockPayload : public BlockPayload {
 public:
  MergeBlockPayload(
      uint32_t numRows,
      std::vector<std::shared_ptr<arrow::Buffer>> buffers,
      const std::vector<bool>* isValidityBuffer,
      arrow::MemoryPool* pool,
      arrow::util::Codec* codec)
      : BlockPayload(Type::kUncompressed, numRows, std::move(buffers), isValidityBuffer, pool, codec) {}

  static arrow::Result<std::unique_ptr<MergeBlockPayload>> merge(
      std::unique_ptr<MergeBlockPayload> source,
      uint32_t appendNumRows,
      std::vector<std::shared_ptr<arrow::Buffer>> appendBuffers,
      arrow::MemoryPool* pool,
      arrow::util::Codec* codec);

  arrow::Result<std::unique_ptr<BlockPayload>> toBlockPayload(Payload::Type payloadType);
};

class UncompressedDiskBlockPayload : public Payload {
 public:
  UncompressedDiskBlockPayload(
      Type type,
      uint32_t numRows,
      const std::vector<bool>* isValidityBuffer,
      arrow::io::InputStream*& inputStream,
      uint64_t rawSize,
      arrow::MemoryPool* pool,
      arrow::util::Codec* codec);

  arrow::Result<std::shared_ptr<arrow::Buffer>> readBufferAt(uint32_t index) override;

  arrow::Status serialize(arrow::io::OutputStream* outputStream) override;

 private:
  arrow::io::InputStream*& inputStream_;
  uint64_t rawSize_;
  arrow::MemoryPool* pool_;
  arrow::util::Codec* codec_;
  uint32_t readPos_{0};

  arrow::Result<std::shared_ptr<arrow::Buffer>> readUncompressedBuffer();
};

class CompressedDiskBlockPayload : public Payload {
 public:
  CompressedDiskBlockPayload(
      uint32_t numRows,
      const std::vector<bool>* isValidityBuffer,
      arrow::io::InputStream*& inputStream,
      uint64_t rawSize,
      arrow::MemoryPool* pool);

  arrow::Status serialize(arrow::io::OutputStream* outputStream) override;

  arrow::Result<std::shared_ptr<arrow::Buffer>> readBufferAt(uint32_t index) override;

 private:
  arrow::io::InputStream*& inputStream_;
  uint64_t rawSize_;
  arrow::MemoryPool* pool_;
};
} // namespace gluten
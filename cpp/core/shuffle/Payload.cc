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
#include "shuffle/Payload.h"

#include <arrow/buffer.h>
#include <arrow/io/memory.h>
#include <arrow/util/bitmap.h>
#include <iostream>
#include <numeric>

#include "shuffle/Options.h"
#include "shuffle/Utils.h"
#include "utils/exception.h"

namespace gluten {

namespace {

static const Payload::Type kCompressedType = Payload::Type::kCompressed;
static const Payload::Type kUncompressedType = Payload::Type::kUncompressed;

template <typename T>
void write(uint8_t** dst, T data) {
  auto ptr = reinterpret_cast<T*>(*dst);
  *ptr = data;
  *dst += sizeof(T);
}

template <typename T>
T* advance(uint8_t** dst) {
  auto ptr = reinterpret_cast<T*>(*dst);
  *dst += sizeof(T);
  return ptr;
}

arrow::Result<int64_t> compressBuffer(
    const std::shared_ptr<arrow::Buffer>& buffer,
    uint8_t*& output,
    int64_t outputLength,
    arrow::util::Codec* codec) {
  if (!buffer) {
    write<int64_t>(&output, kNullBuffer);
    return sizeof(int64_t);
  }
  if (buffer->size() == 0) {
    write<int64_t>(&output, kZeroLengthBuffer);
    return sizeof(int64_t);
  }
  static const int64_t kCompressedBufferHeaderLength = 2 * sizeof(int64_t);
  auto* compressedLengthPtr = advance<int64_t>(&output);
  write(&output, static_cast<int64_t>(buffer->size()));
  ARROW_ASSIGN_OR_RAISE(auto compressedLength, codec->Compress(buffer->size(), buffer->data(), outputLength, output));
  if (compressedLength > buffer->size()) {
    // Write uncompressed buffer.
    memcpy(output, buffer->data(), buffer->size());
    output += buffer->size();
    *compressedLengthPtr = kUncompressedBuffer;
    return kCompressedBufferHeaderLength + buffer->size();
  }
  output += compressedLength;
  *compressedLengthPtr = static_cast<int64_t>(compressedLength);
  return kCompressedBufferHeaderLength + compressedLength;
}

arrow::Status compressAndFlush(
    const std::shared_ptr<arrow::Buffer>& buffer,
    arrow::io::OutputStream* outputStream,
    arrow::util::Codec* codec,
    arrow::MemoryPool* pool) {
  if (!buffer) {
    RETURN_NOT_OK(outputStream->Write(&kNullBuffer, sizeof(int64_t)));
    return arrow::Status::OK();
  }
  if (buffer->size() == 0) {
    RETURN_NOT_OK(outputStream->Write(&kZeroLengthBuffer, sizeof(int64_t)));
    return arrow::Status::OK();
  }
  auto maxCompressedLength = codec->MaxCompressedLen(buffer->size(), buffer->data());
  ARROW_ASSIGN_OR_RAISE(
      std::shared_ptr<arrow::ResizableBuffer> compressed,
      arrow::AllocateResizableBuffer(sizeof(int64_t) * 2 + maxCompressedLength, pool));
  auto output = compressed->mutable_data();
  ARROW_ASSIGN_OR_RAISE(auto compressedSize, compressBuffer(buffer, output, maxCompressedLength, codec));
  RETURN_NOT_OK(outputStream->Write(compressed->data(), compressedSize));
  return arrow::Status::OK();
}

class BitOutputStream {
 public:
  BitOutputStream(arrow::io::OutputStream* os) : os_(os) {}

  arrow::Status write(const uint8_t* source, uint64_t numBits) {
    if (writePos_ == 0) {
      // If already aligned, floor the numBits to 8 and write in bytes.
      auto bytes = arrow::bit_util::BytesForBits(numBits & ~7);
      RETURN_NOT_OK(os_->Write(source, bytes));
      // Record the remaining bits.
      writePos_ = numBits % 8;
      if (writePos_ > 0) {
        data_ = source[bytes] & 7;
      }
      return arrow::Status::OK();
    }
    for (auto i = 0; i < numBits; ++i) {
      RETURN_NOT_OK(writeBit(arrow::bit_util::GetBit(source, i)));
    }
    return arrow::Status::OK();
  }

  arrow::Status end() {
    if (writePos_ != 0) {
      // Write last byte.
      RETURN_NOT_OK(os_->Write(&data_, 1));
    }
    return arrow::Status::OK();
  }

 private:
  uint8_t writePos_{0};
  uint8_t data_{0};
  arrow::io::OutputStream* os_;

  void rewind() {
    writePos_ = 0;
    data_ = 0;
  }

  arrow::Status writeBit(bool setBit) {
    if (setBit) {
      arrow::bit_util::SetBit(&data_, writePos_);
    }
    if ((++writePos_ ^ 8) == 0) {
      RETURN_NOT_OK(os_->Write(&data_, 1));
      rewind();
    }
    return arrow::Status::OK();
  }
};

} // namespace

arrow::Result<std::pair<int32_t, uint32_t>> Payload::readTypeAndRows(arrow::io::InputStream* inputStream) {
  int32_t type;
  uint32_t numRows;
  ARROW_ASSIGN_OR_RAISE(auto pos, inputStream->Tell());
  RETURN_NOT_OK(inputStream->Read(sizeof(Type), &type));
  RETURN_NOT_OK(inputStream->Read(sizeof(uint32_t), &numRows));
  if (type == -2) {
    std::cout << "current pos " << pos << std::endl;
  }
  return std::make_pair(type, numRows);
}

BlockPayload::BlockPayload(
    Payload::Type type,
    uint32_t numRows,
    const std::vector<bool>* isValidityBuffer,
    arrow::MemoryPool* pool,
    arrow::util::Codec* codec,
    std::vector<std::shared_ptr<arrow::Buffer>> buffers)
    : Payload(type, numRows, isValidityBuffer, pool), codec_(codec), buffers_(std::move(buffers)) {}

arrow::Result<std::unique_ptr<BlockPayload>> BlockPayload::fromBuffers(
    Payload::Type type,
    uint32_t numRows,
    std::vector<std::shared_ptr<arrow::Buffer>> buffers,
    const std::vector<bool>* isValidityBuffer,
    arrow::MemoryPool* pool,
    arrow::util::Codec* codec,
    bool reuseBuffers) {
  if (type == Payload::Type::kCompressed) {
    // Compress.
    // Compressed buffer layout: | buffer1 compressedLength | buffer1 uncompressedLength | buffer1 | ...
    auto metadataLength = sizeof(int64_t) * 2 * buffers.size();
    int64_t totalCompressedLength =
        std::accumulate(buffers.begin(), buffers.end(), 0LL, [&](auto sum, const auto& buffer) {
          if (!buffer) {
            return sum;
          }
          return sum + codec->MaxCompressedLen(buffer->size(), buffer->data());
        });
    ARROW_ASSIGN_OR_RAISE(
        std::shared_ptr<arrow::ResizableBuffer> compressed,
        arrow::AllocateResizableBuffer(metadataLength + totalCompressedLength, pool));
    auto output = compressed->mutable_data();
    int64_t actualLength = 0;

    // Compress buffers one by one.
    for (auto& buffer : buffers) {
      auto availableLength = compressed->size() - actualLength;
      // Release buffer after compression.
      ARROW_ASSIGN_OR_RAISE(auto compressedSize, compressBuffer(std::move(buffer), output, availableLength, codec));
      actualLength += compressedSize;
    }

    ARROW_RETURN_IF(
        compressed->size() < actualLength, arrow::Status::Invalid("Writing compressed buffer out of bound."));
    RETURN_NOT_OK(compressed->Resize(actualLength));
    return std::make_unique<BlockPayload>(
        Type::kCompressed,
        numRows,
        isValidityBuffer,
        pool,
        codec,
        std::vector<std::shared_ptr<arrow::Buffer>>{compressed});
  }
  if (reuseBuffers) {
    // Copy source buffers.
    std::vector<std::shared_ptr<arrow::Buffer>> copies;
    for (auto& buffer : buffers) {
      if (!buffer) {
        copies.push_back(nullptr);
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(auto copy, arrow::AllocateResizableBuffer(buffer->size(), pool));
      memcpy(copy->mutable_data(), buffer->data(), buffer->size());
      copies.push_back(std::move(copy));
    }
    return std::make_unique<BlockPayload>(
        Type::kUncompressed, numRows, isValidityBuffer, pool, codec, std::move(copies));
  }
  // Move source buffers.
  return std::make_unique<BlockPayload>(
      Type::kUncompressed, numRows, isValidityBuffer, pool, codec, std::move(buffers));
}

arrow::Status BlockPayload::serialize(arrow::io::OutputStream* outputStream) {
  if (type_ == Type::kUncompressed) {
    RETURN_NOT_OK(outputStream->Write(&kUncompressedType, sizeof(Type)));
    RETURN_NOT_OK(outputStream->Write(&numRows_, sizeof(uint32_t)));
    for (auto& buffer : buffers_) {
      if (!buffer) {
        RETURN_NOT_OK(outputStream->Write(&kNullBuffer, sizeof(int64_t)));
        continue;
      }
      int64_t bufferSize = buffer->size();
      RETURN_NOT_OK(outputStream->Write(&bufferSize, sizeof(int64_t)));
      if (bufferSize > 0) {
        RETURN_NOT_OK(outputStream->Write(std::move(buffer)));
      }
    }
  } else {
    RETURN_NOT_OK(outputStream->Write(&kCompressedType, sizeof(Type)));
    RETURN_NOT_OK(outputStream->Write(&numRows_, sizeof(uint32_t)));
    RETURN_NOT_OK(outputStream->Write(std::move(buffers_[0])));
  }
  buffers_.clear();
  return arrow::Status::OK();
}

arrow::Result<std::vector<std::shared_ptr<arrow::Buffer>>> BlockPayload::deserialize(
    arrow::io::InputStream* inputStream,
    const std::shared_ptr<arrow::Schema>& schema,
    const std::shared_ptr<arrow::util::Codec>& codec,
    arrow::MemoryPool* pool,
    uint32_t& numRows) {
  static const std::vector<std::shared_ptr<arrow::Buffer>> kEmptyBuffers{};
  ARROW_ASSIGN_OR_RAISE(auto typeAndRows, readTypeAndRows(inputStream));
  if (typeAndRows.first == kIpcContinuationToken && typeAndRows.second == kZeroLength) {
    numRows = 0;
    return kEmptyBuffers;
  }
  numRows = typeAndRows.second;
  auto fields = schema->fields();

  auto isCompressionEnabled = typeAndRows.first == Type::kUncompressed || codec == nullptr;
  auto readBuffer = [&]() {
    if (isCompressionEnabled) {
      return readUncompressedBuffer(inputStream);
    } else {
      return readCompressedBuffer(inputStream, codec, pool);
    }
  };

  bool hasComplexDataType = false;
  std::vector<std::shared_ptr<arrow::Buffer>> buffers;
  for (const auto& field : fields) {
    auto fieldType = field->type()->id();
    switch (fieldType) {
      case arrow::BinaryType::type_id:
      case arrow::StringType::type_id: {
        buffers.emplace_back();
        ARROW_ASSIGN_OR_RAISE(buffers.back(), readBuffer());
        buffers.emplace_back();
        ARROW_ASSIGN_OR_RAISE(buffers.back(), readBuffer());
        buffers.emplace_back();
        ARROW_ASSIGN_OR_RAISE(buffers.back(), readBuffer());
        break;
      }
      case arrow::StructType::type_id:
      case arrow::MapType::type_id:
      case arrow::ListType::type_id: {
        hasComplexDataType = true;
      } break;
      default: {
        buffers.emplace_back();
        ARROW_ASSIGN_OR_RAISE(buffers.back(), readBuffer());
        buffers.emplace_back();
        ARROW_ASSIGN_OR_RAISE(buffers.back(), readBuffer());
        break;
      }
    }
  }
  if (hasComplexDataType) {
    buffers.emplace_back();
    ARROW_ASSIGN_OR_RAISE(buffers.back(), readBuffer());
  }
  return buffers;
}

arrow::Result<std::shared_ptr<arrow::Buffer>> BlockPayload::readUncompressedBuffer(
    arrow::io::InputStream* inputStream) {
  int64_t bufferLength;
  RETURN_NOT_OK(inputStream->Read(sizeof(int64_t), &bufferLength));
  if (bufferLength == kNullBuffer) {
    return nullptr;
  }
  if (bufferLength == kZeroLengthBuffer) {
    return zeroLengthNullBuffer();
  }
  ARROW_ASSIGN_OR_RAISE(auto buffer, inputStream->Read(bufferLength));
  return buffer;
}

arrow::Result<std::shared_ptr<arrow::Buffer>> BlockPayload::readCompressedBuffer(
    arrow::io::InputStream* inputStream,
    const std::shared_ptr<arrow::util::Codec>& codec,
    arrow::MemoryPool* pool) {
  int64_t compressedLength;
  RETURN_NOT_OK(inputStream->Read(sizeof(int64_t), &compressedLength));
  if (compressedLength == kNullBuffer) {
    return nullptr;
  }
  if (compressedLength == kZeroLengthBuffer) {
    return zeroLengthNullBuffer();
  }

  int64_t uncompressedLength;
  RETURN_NOT_OK(inputStream->Read(sizeof(int64_t), &uncompressedLength));
  if (compressedLength == kUncompressedBuffer) {
    ARROW_ASSIGN_OR_RAISE(auto uncompressed, arrow::AllocateBuffer(uncompressedLength, pool));
    RETURN_NOT_OK(inputStream->Read(uncompressedLength, const_cast<uint8_t*>(uncompressed->data())));
    return uncompressed;
  }
  ARROW_ASSIGN_OR_RAISE(auto compressed, arrow::AllocateBuffer(compressedLength, pool));
  RETURN_NOT_OK(inputStream->Read(compressedLength, const_cast<uint8_t*>(compressed->data())));
  ARROW_ASSIGN_OR_RAISE(auto output, arrow::AllocateBuffer(uncompressedLength, pool));
  RETURN_NOT_OK(codec->Decompress(
      compressedLength, compressed->data(), uncompressedLength, const_cast<uint8_t*>(output->data())));
  return output;
}

arrow::Result<std::shared_ptr<arrow::Buffer>> BlockPayload::readBufferAt(uint32_t pos) {
  if (type_ == Type::kCompressed) {
    return arrow::Status::Invalid("Cannot read buffer from compressed BlockPayload.");
  }
  return std::move(buffers_[pos]);
}

GroupPayload::GroupPayload(
    Payload::Type type,
    uint32_t numRows,
    const std::vector<bool>* isValidityBuffer,
    arrow::MemoryPool* pool,
    arrow::util::Codec* codec,
    std::vector<std::unique_ptr<Payload>> payloads)
    : Payload(type, numRows, isValidityBuffer, pool), codec_(codec) {
  if (payloads.size() <= 1) {
    throw GlutenException("Cannot create GroupPayload from number of payloads <= 1");
  }
  auto numBuffers = isValidityBuffer->size();
  buffers_.resize(numBuffers);
  isValidityAllNull_.resize(numBuffers, true);
  for (auto& payload : payloads) {
    bufferNumRows_.push_back(payload->numRows());
  }

  for (size_t i = 0; i < numBuffers; ++i) {
    if (isValidityBuffer->at(i)) {
      for (auto& payload : payloads) {
        GLUTEN_ASSIGN_OR_THROW(auto buffer, payload->readBufferAt(i));
        if (buffer) {
          isValidityAllNull_[i] = false;
        }
        buffers_[i].push_back(std::move(buffer));
      }
      continue;
    }
    for (auto& payload : payloads) {
      GLUTEN_ASSIGN_OR_THROW(auto buffer, payload->readBufferAt(i));
      buffers_[i].push_back(std::move(buffer));
    }
  }
}

arrow::Status GroupPayload::serialize(arrow::io::OutputStream* outputStream) {
  if (type_ == Payload::Type::kUncompressed) {
    return serializeUncompressed(outputStream);
  }
  return serializeCompressed(outputStream);
}

arrow::Result<std::shared_ptr<arrow::Buffer>> GroupPayload::readBufferAt(uint32_t index) {
  auto rawSize = rawSizeAt(index);
  if (rawSize == kNullBuffer) {
    return nullptr;
  }
  if (rawSize == 0) {
    return zeroLengthNullBuffer();
  }
  ARROW_ASSIGN_OR_RAISE(auto bufferOs, arrow::io::BufferOutputStream::Create(rawSize, pool_));
  if (isValidityBuffer_->at(index)) {
    RETURN_NOT_OK(writeValidityBuffer(bufferOs.get(), index));
  } else {
    RETURN_NOT_OK(writeBuffer(bufferOs.get(), index));
  }
  return bufferOs->Finish();
}

int64_t GroupPayload::rawSizeAt(uint32_t index) {
  if (isValidityBuffer_->at(index)) {
    // Need to handle Validity buf specially.
    if (isValidityAllNull_[index]) {
      return kNullBuffer;
    }
    auto numRows = std::accumulate(bufferNumRows_.begin(), bufferNumRows_.end(), 0);
    return arrow::bit_util::BytesForBits(numRows);
  }
  auto rawSize =
      std::accumulate(buffers_[index].begin(), buffers_[index].end(), 0LL, [&](auto sum, const auto& buffer) {
        return sum + buffer->size();
      });
  return rawSize;
}

const arrow::Buffer* GroupPayload::validityBufferAllTrue() {
  thread_local std::unique_ptr<arrow::Buffer> validityBuffer;
  if (validityBuffer) {
    return validityBuffer.get();
  }
  // 512 bytes should be enough capacity for batch size <= 4k.
  auto result = arrow::AllocateResizableBuffer(512, pool_);
  if (result.ok()) {
    throw GlutenException("Failed to allocate static validity buffer");
  }
  validityBuffer = result.MoveValueUnsafe();
  memset(validityBuffer->mutable_data(), validityBuffer->size(), 0xff);
  return validityBuffer.get();
}

arrow::Status GroupPayload::writeValidityBuffer(arrow::io::OutputStream* outputStream, uint32_t index) {
  auto bitOs = BitOutputStream(outputStream);
  for (auto& buffer : buffers_[index]) {
    if (!buffer) {
      // Write all true.
      auto remainingRows = bufferNumRows_[index];
      auto validityBuffer = validityBufferAllTrue();
      auto rowsPerRun = validityBuffer->size() << 3;
      while (remainingRows > rowsPerRun) {
        RETURN_NOT_OK(bitOs.write(validityBuffer->data(), rowsPerRun));
        remainingRows -= rowsPerRun;
      }
      RETURN_NOT_OK(bitOs.write(validityBufferAllTrue()->data(), remainingRows));
    } else {
      RETURN_NOT_OK(bitOs.write(buffer->data(), bufferNumRows_[index]));
      buffer = nullptr;
    }
  }
  RETURN_NOT_OK(bitOs.end());
  return arrow::Status::OK();
}

arrow::Status GroupPayload::writeBuffer(arrow::io::OutputStream* outputStream, uint32_t index) {
  for (auto& buffer : buffers_[index]) {
    if (buffer->size() > 0) {
      RETURN_NOT_OK(outputStream->Write(std::move(buffer)));
    }
    // Skip writing zero length buffer, such as empty value buffer of binary array.
  }
  return arrow::Status::OK();
}

arrow::Status GroupPayload::serializeUncompressed(arrow::io::OutputStream* outputStream) {
  // Otherwise type is either kToBeCompressed or kToBeMerged.
  // TODO: Support reading and merging kToBeMerged
  RETURN_NOT_OK(outputStream->Write(&kUncompressedType, sizeof(Type)));
  RETURN_NOT_OK(outputStream->Write(&numRows_, sizeof(uint32_t)));
  for (size_t i = 0; i < buffers_.size(); ++i) {
    auto rawSize = rawSizeAt(i);
    if (rawSize == kNullBuffer || rawSize == 0) {
      RETURN_NOT_OK(outputStream->Write(&rawSize, sizeof(int64_t)));
      continue;
    }
    if (isValidityBuffer_->at(i)) {
      RETURN_NOT_OK(writeValidityBuffer(outputStream, i));
    } else {
      RETURN_NOT_OK(writeBuffer(outputStream, i));
    }
  }
  return arrow::Status::OK();
}

arrow::Status GroupPayload::serializeCompressed(arrow::io::OutputStream* outputStream) {
  RETURN_NOT_OK(outputStream->Write(&kCompressedType, sizeof(Type)));
  RETURN_NOT_OK(outputStream->Write(&numRows_, sizeof(uint32_t)));
  for (auto i = 0; i < numBuffers(); ++i) {
    ARROW_ASSIGN_OR_RAISE(auto merged, readBufferAt(i));
    RETURN_NOT_OK(compressAndFlush(std::move(merged), outputStream, codec_, pool_));
  }
  return arrow::Status::OK();
}

UncompressedDiskBlockPayload::UncompressedDiskBlockPayload(
    Payload::Type type,
    uint32_t numRows,
    const std::vector<bool>* isValidityBuffer,
    arrow::MemoryPool* pool,
    arrow::io::InputStream*& inputStream,
    uint64_t rawSize,
    arrow::util::Codec* codec)
    : Payload(type, numRows, isValidityBuffer, pool),
      inputStream_(inputStream),
      rawSize_(rawSize),
      codec_(codec) {}

arrow::Result<std::shared_ptr<arrow::Buffer>> UncompressedDiskBlockPayload::readBufferAt(uint32_t index) {
  return arrow::Status::Invalid("Cannot read buffer from UncompressedDiskBlockPayload.");
}

arrow::Status UncompressedDiskBlockPayload::serialize(arrow::io::OutputStream* outputStream) {
  if (codec_ == nullptr) {
    ARROW_ASSIGN_OR_RAISE(auto block, inputStream_->Read(rawSize_));
    RETURN_NOT_OK(outputStream->Write(block));
    return arrow::Status::OK();
  }

  ARROW_ASSIGN_OR_RAISE(auto startPos, inputStream_->Tell());
  // TODO: For kToBeMerged, read, merge, compress and write.
  auto typeAndRows = readTypeAndRows(inputStream_);
  // Discard type and rows.
  RETURN_NOT_OK(typeAndRows.status());
  RETURN_NOT_OK(outputStream->Write(&kCompressedType, sizeof(Type)));
  RETURN_NOT_OK(outputStream->Write(&numRows_, sizeof(uint32_t)));
  auto readPos = startPos;
  while (readPos - startPos < rawSize_) {
    ARROW_ASSIGN_OR_RAISE(auto uncompressed, readUncompressedBuffer());
    ARROW_ASSIGN_OR_RAISE(readPos, inputStream_->Tell());
    RETURN_NOT_OK(compressAndFlush(std::move(uncompressed), outputStream, codec_, pool_));
  }
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::Buffer>> UncompressedDiskBlockPayload::readUncompressedBuffer() {
  readPos_++;
  int64_t bufferLength;
  RETURN_NOT_OK(inputStream_->Read(sizeof(int64_t), &bufferLength));
  if (bufferLength == kNullBuffer) {
    return nullptr;
  }
  if (bufferLength == 0) {
    return zeroLengthNullBuffer();
  }
  ARROW_ASSIGN_OR_RAISE(auto buffer, inputStream_->Read(bufferLength));
  return buffer;
}

CompressedDiskBlockPayload::CompressedDiskBlockPayload(
    uint32_t numRows,
    const std::vector<bool>* isValidityBuffer,
    arrow::MemoryPool* pool,
    arrow::io::InputStream*& inputStream,
    uint64_t rawSize)
    : Payload(Type::kCompressed, numRows, isValidityBuffer, pool),
      inputStream_(inputStream),
      rawSize_(rawSize) {}

arrow::Status CompressedDiskBlockPayload::serialize(arrow::io::OutputStream* outputStream) {
  ARROW_ASSIGN_OR_RAISE(auto block, inputStream_->Read(rawSize_));
  RETURN_NOT_OK(outputStream->Write(block));
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::Buffer>> CompressedDiskBlockPayload::readBufferAt(uint32_t index) {
  return arrow::Status::Invalid("Cannot read buffer from CompressedDiskBlockPayload.");
}
} // namespace gluten
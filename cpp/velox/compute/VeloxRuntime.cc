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

#include "VeloxRuntime.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>

#include "VeloxBackend.h"
#include "compute/ResultIterator.h"
#include "compute/Runtime.h"
#include "compute/VeloxPlanConverter.h"
#include "config/VeloxConfig.h"
#include "operators/serializer/VeloxRowToColumnarConverter.h"
#include "operators/writer/VeloxArrowWriter.h"
#include "shuffle/VeloxShuffleReader.h"
#include "shuffle/VeloxShuffleWriter.h"
#include "utils/ConfigExtractor.h"
#include "utils/VeloxArrowUtils.h"

#ifdef ENABLE_HDFS
#include "operators/writer/VeloxParquetDataSourceHDFS.h"
#endif

#ifdef ENABLE_S3
#include "operators/writer/VeloxParquetDataSourceS3.h"
#endif

#ifdef ENABLE_GCS
#include "operators/writer/VeloxParquetDataSourceGCS.h"
#endif

#ifdef ENABLE_ABFS
#include "operators/writer/VeloxParquetDataSourceABFS.h"
#endif

using namespace facebook;

namespace gluten {

VeloxRuntime::VeloxRuntime(
    const std::string& kind,
    VeloxMemoryManager* vmm,
    const std::unordered_map<std::string, std::string>& confMap)
    : Runtime(kind, vmm, confMap) {
  // Refresh session config.
  veloxCfg_ =
      std::make_shared<facebook::velox::config::ConfigBase>(std::unordered_map<std::string, std::string>(confMap_));
  debugModeEnabled_ = veloxCfg_->get<bool>(kDebugModeEnabled, false);
  FLAGS_minloglevel = veloxCfg_->get<uint32_t>(kGlogSeverityLevel, FLAGS_minloglevel);
  FLAGS_v = veloxCfg_->get<uint32_t>(kGlogVerboseLevel, FLAGS_v);
}

void VeloxRuntime::parsePlan(const uint8_t* data, int32_t size, std::optional<std::string> dumpFile) {
  if (debugModeEnabled_ || dumpFile.has_value()) {
    try {
      auto planJson = substraitFromPbToJson("Plan", data, size, dumpFile);
      LOG_IF(INFO, debugModeEnabled_) << std::string(50, '#') << " received substrait::Plan: " << taskInfo_ << std::endl
                                      << planJson;
    } catch (const std::exception& e) {
      LOG(WARNING) << "Error converting Substrait plan to JSON: " << e.what();
    }
  }

  GLUTEN_CHECK(parseProtobuf(data, size, &substraitPlan_) == true, "Parse substrait plan failed");
}

void VeloxRuntime::parseSplitInfo(const uint8_t* data, int32_t size, std::optional<std::string> dumpFile) {
  if (debugModeEnabled_ || dumpFile.has_value()) {
    try {
      auto splitJson = substraitFromPbToJson("ReadRel.LocalFiles", data, size, dumpFile);
      LOG_IF(INFO, debugModeEnabled_) << std::string(50, '#')
                                      << " received substrait::ReadRel.LocalFiles: " << taskInfo_ << std::endl
                                      << splitJson;
    } catch (const std::exception& e) {
      LOG(WARNING) << "Error converting Substrait plan to JSON: " << e.what();
    }
  }
  ::substrait::ReadRel_LocalFiles localFile;
  GLUTEN_CHECK(parseProtobuf(data, size, &localFile) == true, "Parse substrait plan failed");
  localFiles_.push_back(localFile);
}

void VeloxRuntime::getInfoAndIds(
    const std::unordered_map<velox::core::PlanNodeId, std::shared_ptr<SplitInfo>>& splitInfoMap,
    const std::unordered_set<velox::core::PlanNodeId>& leafPlanNodeIds,
    std::vector<std::shared_ptr<SplitInfo>>& scanInfos,
    std::vector<velox::core::PlanNodeId>& scanIds,
    std::vector<velox::core::PlanNodeId>& streamIds) {
  for (const auto& leafPlanNodeId : leafPlanNodeIds) {
    auto it = splitInfoMap.find(leafPlanNodeId);
    if (it == splitInfoMap.end()) {
      throw std::runtime_error("Could not find leafPlanNodeId.");
    }
    auto splitInfo = it->second;
    if (splitInfo->isStream) {
      streamIds.emplace_back(leafPlanNodeId);
    } else {
      scanInfos.emplace_back(splitInfo);
      scanIds.emplace_back(leafPlanNodeId);
    }
  }
}

std::string VeloxRuntime::planString(bool details, const std::unordered_map<std::string, std::string>& sessionConf) {
  std::vector<std::shared_ptr<ResultIterator>> inputs;
  auto veloxMemoryPool = gluten::defaultLeafVeloxMemoryPool();
  VeloxPlanConverter veloxPlanConverter(inputs, veloxMemoryPool.get(), sessionConf, std::nullopt, true);
  auto veloxPlan = veloxPlanConverter.toVeloxPlan(substraitPlan_, localFiles_);
  return veloxPlan->toString(details, true);
}

VeloxMemoryManager* VeloxRuntime::memoryManager() {
  auto vmm = dynamic_cast<VeloxMemoryManager*>(memoryManager_);
  GLUTEN_CHECK(vmm != nullptr, "Not a Velox memory manager");
  return vmm;
}

std::shared_ptr<ResultIterator> VeloxRuntime::createResultIterator(
    const std::string& spillDir,
    const std::vector<std::shared_ptr<ResultIterator>>& inputs,
    const std::unordered_map<std::string, std::string>& sessionConf) {
  LOG_IF(INFO, debugModeEnabled_) << "VeloxRuntime session config:" << printConfig(confMap_);

  VeloxPlanConverter veloxPlanConverter(
      inputs, memoryManager()->getLeafMemoryPool().get(), sessionConf, *localWriteFilesTempPath());
  veloxPlan_ = veloxPlanConverter.toVeloxPlan(substraitPlan_, std::move(localFiles_));

  // Scan node can be required.
  std::vector<std::shared_ptr<SplitInfo>> scanInfos;
  std::vector<velox::core::PlanNodeId> scanIds;
  std::vector<velox::core::PlanNodeId> streamIds;

  // Separate the scan ids and stream ids, and get the scan infos.
  getInfoAndIds(veloxPlanConverter.splitInfos(), veloxPlan_->leafPlanNodeIds(), scanInfos, scanIds, streamIds);

  auto wholestageIter = std::make_unique<WholeStageResultIterator>(
      memoryManager(), veloxPlan_, scanIds, scanInfos, streamIds, spillDir, sessionConf, taskInfo_);
  return std::make_shared<ResultIterator>(std::move(wholestageIter), this);
}

std::shared_ptr<ColumnarToRowConverter> VeloxRuntime::createColumnar2RowConverter(int64_t column2RowMemThreshold) {
  auto veloxPool = memoryManager()->getLeafMemoryPool();
  return std::make_shared<VeloxColumnarToRowConverter>(veloxPool, column2RowMemThreshold);
}

std::shared_ptr<ColumnarBatch> VeloxRuntime::createOrGetEmptySchemaBatch(int32_t numRows) {
  auto& lookup = emptySchemaBatchLoopUp_;
  if (lookup.find(numRows) == lookup.end()) {
    auto veloxPool = memoryManager()->getLeafMemoryPool();
    const std::shared_ptr<VeloxColumnarBatch>& batch =
        VeloxColumnarBatch::from(veloxPool.get(), gluten::createZeroColumnBatch(numRows));
    lookup.emplace(numRows, batch); // the batch will be released after Spark task ends
  }
  return lookup.at(numRows);
}

std::shared_ptr<ColumnarBatch> VeloxRuntime::select(
    std::shared_ptr<ColumnarBatch> batch,
    const std::vector<int32_t>& columnIndices) {
  auto veloxPool = memoryManager()->getLeafMemoryPool();
  auto veloxBatch = gluten::VeloxColumnarBatch::from(veloxPool.get(), batch);
  auto outputBatch = veloxBatch->select(veloxPool.get(), std::move(columnIndices));
  return outputBatch;
}

std::shared_ptr<RowToColumnarConverter> VeloxRuntime::createRow2ColumnarConverter(struct ArrowSchema* cSchema) {
  auto veloxPool = memoryManager()->getLeafMemoryPool();
  return std::make_shared<VeloxRowToColumnarConverter>(cSchema, veloxPool);
}

std::shared_ptr<ShuffleWriter> VeloxRuntime::createShuffleWriter(
    int numPartitions,
    std::unique_ptr<PartitionWriter> partitionWriter,
    ShuffleWriterOptions options) {
  auto veloxPool = memoryManager()->getLeafMemoryPool();
  auto arrowPool = memoryManager()->getArrowMemoryPool();
  GLUTEN_ASSIGN_OR_THROW(
      std::shared_ptr<ShuffleWriter> shuffleWriter,
      VeloxShuffleWriter::create(
          options.shuffleWriterType,
          numPartitions,
          std::move(partitionWriter),
          std::move(options),
          veloxPool,
          arrowPool));
  return shuffleWriter;
}

std::shared_ptr<VeloxDataSource> VeloxRuntime::createDataSource(
    const std::string& filePath,
    std::shared_ptr<arrow::Schema> schema) {
  static std::atomic_uint32_t id{0UL};
  auto veloxPool = memoryManager()->getAggregateMemoryPool()->addAggregateChild("datasource." + std::to_string(id++));
  // Pass a dedicate pool for S3 and GCS sinks as can't share veloxPool
  // with parquet writer.
  // FIXME: Check file formats?
  auto sinkPool = memoryManager()->getLeafMemoryPool();
  if (isSupportedHDFSPath(filePath)) {
#ifdef ENABLE_HDFS
    return std::make_shared<VeloxParquetDataSourceHDFS>(filePath, veloxPool, sinkPool, schema);
#else
    throw std::runtime_error(
        "The write path is hdfs path but the HDFS haven't been enabled when writing parquet data in velox runtime!");
#endif
  } else if (isSupportedS3SdkPath(filePath)) {
#ifdef ENABLE_S3
    return std::make_shared<VeloxParquetDataSourceS3>(filePath, veloxPool, sinkPool, schema);
#else
    throw std::runtime_error(
        "The write path is S3 path but the S3 haven't been enabled when writing parquet data in velox runtime!");
#endif
  } else if (isSupportedGCSPath(filePath)) {
#ifdef ENABLE_GCS
    return std::make_shared<VeloxParquetDataSourceGCS>(filePath, veloxPool, sinkPool, schema);
#else
    throw std::runtime_error(
        "The write path is GCS path but the GCS haven't been enabled when writing parquet data in velox runtime!");
#endif
  } else if (isSupportedABFSPath(filePath)) {
#ifdef ENABLE_ABFS
    return std::make_shared<VeloxParquetDataSourceABFS>(filePath, veloxPool, sinkPool, schema);
#else
    throw std::runtime_error(
        "The write path is ABFS path but the ABFS haven't been enabled when writing parquet data in velox runtime!");
#endif
  }
  return std::make_shared<VeloxParquetDataSource>(filePath, veloxPool, sinkPool, schema);
}

std::shared_ptr<ShuffleReader> VeloxRuntime::createShuffleReader(
    std::shared_ptr<arrow::Schema> schema,
    ShuffleReaderOptions options) {
  auto rowType = facebook::velox::asRowType(gluten::fromArrowSchema(schema));
  auto codec = gluten::createArrowIpcCodec(options.compressionType, options.codecBackend);
  auto ctxVeloxPool = memoryManager()->getLeafMemoryPool();
  auto veloxCompressionType = facebook::velox::common::stringToCompressionKind(options.compressionTypeStr);
  auto deserializerFactory = std::make_unique<gluten::VeloxShuffleReaderDeserializerFactory>(
      schema,
      std::move(codec),
      veloxCompressionType,
      rowType,
      options.batchSize,
      options.bufferSize,
      memoryManager()->getArrowMemoryPool(),
      ctxVeloxPool,
      options.shuffleWriterType);
  auto reader = std::make_shared<VeloxShuffleReader>(std::move(deserializerFactory));
  return reader;
}

std::unique_ptr<ColumnarBatchSerializer> VeloxRuntime::createColumnarBatchSerializer(struct ArrowSchema* cSchema) {
  auto arrowPool = memoryManager()->getArrowMemoryPool();
  auto veloxPool = memoryManager()->getLeafMemoryPool();
  return std::make_unique<VeloxColumnarBatchSerializer>(arrowPool, veloxPool, cSchema);
}

void VeloxRuntime::dumpConf(const std::string& path) {
  const auto& backendConfMap = VeloxBackend::get()->getBackendConf()->rawConfigs();
  auto allConfMap = backendConfMap;

  for (const auto& pair : confMap_) {
    allConfMap.insert_or_assign(pair.first, pair.second);
  }

  // Open file "velox.conf" for writing, automatically creating it if it doesn't exist,
  // or overwriting it if it does.
  std::ofstream outFile(path);
  if (!outFile.is_open()) {
    LOG(ERROR) << "Failed to open file for writing: " << path;
    return;
  }

  // Calculate the maximum key length for alignment.
  size_t maxKeyLength = 0;
  for (const auto& pair : allConfMap) {
    maxKeyLength = std::max(maxKeyLength, pair.first.length());
  }

  // Write each key-value pair to the file with adjusted spacing for alignment
  outFile << "[Backend Conf]" << std::endl;
  for (const auto& pair : backendConfMap) {
    outFile << std::left << std::setw(maxKeyLength + 1) << pair.first << ' ' << pair.second << std::endl;
  }
  outFile << std::endl << "[Session Conf]" << std::endl;
  for (const auto& pair : confMap_) {
    outFile << std::left << std::setw(maxKeyLength + 1) << pair.first << ' ' << pair.second << std::endl;
  }

  outFile.close();
}

std::shared_ptr<ArrowWriter> VeloxRuntime::createArrowWriter(const std::string& path) {
  int64_t batchSize = 4096;
  if (auto it = confMap_.find(kSparkBatchSize); it != confMap_.end()) {
    batchSize = std::atol(it->second.c_str());
  }
  return std::make_shared<VeloxArrowWriter>(path, batchSize, memoryManager()->getLeafMemoryPool().get());
}

} // namespace gluten

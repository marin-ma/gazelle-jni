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

#include <chrono>
#include <iostream>

using TimePoint = std::chrono::time_point<std::chrono::steady_clock, std::chrono::nanoseconds>;

namespace gluten {
class Timer {
 public:
  explicit Timer() = default;

  void start() {
    running_ = true;
    startTime_ = std::chrono::steady_clock::now();
  }

  void stop() {
    if (!running_) {
      return;
    }
    running_ = false;
    realTimeUsed_ +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - startTime_).count();
  }

  void reset() {
    running_ = false;
    realTimeUsed_ = 0;
  }

  bool running() const {
    return running_;
  }

  // REQUIRES: timer is not running
  int64_t realTimeUsed() const {
    return realTimeUsed_;
  }

 private:
  bool running_ = false; // Is the timer running
  TimePoint startTime_{}; // If running_

  // Accumulated time so far (does not contain current slice if running_)
  int64_t realTimeUsed_ = 0;
};

class ScopedTimer {
 public:
  explicit ScopedTimer(int64_t* toAdd) : toAdd_(toAdd) {
    startInternal();
  }

  ~ScopedTimer() {
    stopInternal();
  }

  void switchTo(int64_t* toAdd) {
    stopInternal();
    toAdd_ = toAdd;
    startInternal();
  }

 private:
  Timer timer_{};
  int64_t* toAdd_;

  void stopInternal() {
    timer_.stop();
    *toAdd_ += timer_.realTimeUsed();
  }

  void startInternal() {
    timer_.reset();
    timer_.start();
  }
};

namespace logger {
static const std::string kDecompressTime = "decompress";
static const std::string kReadStreamTime = "read stream";
static const std::string kMerge = "merge";
static const std::string kArrowToVelox = "arrow to velox";
} // namespace logger

class LogTimer {
 public:
  explicit LogTimer(int64_t threadId, int64_t microOffset, const std::string& name)
      : threadId_(threadId), microOffset_(microOffset), name_(name) {
    startTime_ = std::chrono::system_clock::now();
  }

  void abandon() {
    abandoned_ = true;
  }

  ~LogTimer() {
    if (!abandoned_) {
      auto startTime = std::chrono::duration_cast<std::chrono::microseconds>(startTime_.time_since_epoch());
      auto end = std::chrono::system_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - startTime_).count();
      std::cout << "SHUFFLE_READER_BREAKDOWN: [" << name_ << "]" << threadId_ << " " << startTime.count() - microOffset_
                << " " << duration << std::endl;
    }
  }

 private:
  int64_t threadId_;
  int64_t microOffset_;
  const std::string& name_;
  std::chrono::system_clock::time_point startTime_{}; // If running_

  bool abandoned_{false};
};
} // namespace gluten

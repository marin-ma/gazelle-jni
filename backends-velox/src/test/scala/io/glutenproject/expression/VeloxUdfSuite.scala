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
package io.glutenproject.expression

import org.apache.spark.SparkConf
import org.apache.spark.sql.GlutenQueryTest
import org.apache.spark.sql.test.SharedSparkSession

class VeloxUdfSuite extends GlutenQueryTest with SharedSparkSession {

  override protected def beforeAll(): Unit = {
    super.beforeAll()
    sparkContext.setLogLevel("INFO")
  }
  override protected def sparkConf: SparkConf =
    super.sparkConf
      .set("spark.plugins", "io.glutenproject.GlutenPlugin")
      .set("spark.default.parallelism", "1")
      .set("spark.memory.offHeap.enabled", "true")
      .set("spark.memory.offHeap.size", "1024MB")

  test("test udf") {
    val df = spark.sql("""select myudf1(1), myudf2(100L)""")
    df.collect().sameElements(Array(6, 105))
  }
}

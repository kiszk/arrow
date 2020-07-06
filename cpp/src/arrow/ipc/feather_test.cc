// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "arrow/array.h"
#include "arrow/buffer.h"
#include "arrow/io/memory.h"
#include "arrow/ipc/feather.h"
#include "arrow/ipc/test_common.h"
#include "arrow/record_batch.h"
#include "arrow/status.h"
#include "arrow/table.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/type.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/compression.h"

namespace arrow {

using internal::checked_cast;

namespace ipc {
namespace feather {

struct TestParam {
  TestParam(int arg_version,
            Compression::type arg_compression = Compression::UNCOMPRESSED)
      : version(arg_version), compression(arg_compression) {}

  int version;
  Compression::type compression;
};

class TestFeatherBase {
 public:
  void SetUp() { Initialize(); }

  void Initialize() { ASSERT_OK_AND_ASSIGN(stream_, io::BufferOutputStream::Create()); }

  virtual WriteProperties GetProperties() = 0;

  void DoWrite(const Table& table) {
    Initialize();
    ASSERT_OK(WriteTable(table, stream_.get(), GetProperties()));
    ASSERT_OK_AND_ASSIGN(output_, stream_->Finish());
    auto buffer = std::make_shared<io::BufferReader>(output_);
    ASSERT_OK_AND_ASSIGN(reader_, Reader::Open(buffer));
  }

  void CheckSlice(std::shared_ptr<RecordBatch> batch, int start, int size) {
    batch = batch->Slice(start, size);
    ASSERT_OK_AND_ASSIGN(auto table, Table::FromRecordBatches({batch}));

    DoWrite(*table);
    std::shared_ptr<Table> result;
    ASSERT_OK(reader_->Read(&result));
    if (table->num_rows() > 0) {
      AssertTablesEqual(*table, *result);
    } else {
      ASSERT_EQ(0, result->num_rows());
      ASSERT_TRUE(result->schema()->Equals(*table->schema()));
    }
  }

  void CheckSlices(std::shared_ptr<RecordBatch> batch) {
    std::vector<int> starts = {0, 1, 300, 301, 302, 303, 304, 305, 306, 307};
    std::vector<int> sizes = {0, 1, 7, 8, 30, 32, 100};
    for (auto start : starts) {
      for (auto size : sizes) {
        CheckSlice(batch, start, size);
      }
    }
  }

  void CheckRoundtrip(std::shared_ptr<RecordBatch> batch) {
    std::vector<std::shared_ptr<RecordBatch>> batches = {batch};
    ASSERT_OK_AND_ASSIGN(auto table, Table::FromRecordBatches(batches));

    DoWrite(*table);

    std::shared_ptr<Table> read_table;
    ASSERT_OK(reader_->Read(&read_table));
    AssertTablesEqual(*table, *read_table);
  }

 protected:
  std::shared_ptr<io::BufferOutputStream> stream_;
  std::shared_ptr<Reader> reader_;
  std::shared_ptr<Buffer> output_;
};

class TestFeather : public ::testing::TestWithParam<TestParam>, public TestFeatherBase {
 public:
  void SetUp() { TestFeatherBase::SetUp(); }

  WriteProperties GetProperties() {
    auto param = GetParam();

    auto props = WriteProperties::Defaults();
    props.version = param.version;

    // Don't fail if the build doesn't have LZ4_FRAME or ZSTD enabled
    if (util::Codec::IsAvailable(param.compression)) {
      props.compression = param.compression;
    } else {
      props.compression = Compression::UNCOMPRESSED;
    }
    return props;
  }
};

class TestFeatherRoundTrip : public ::testing::TestWithParam<ipc::test::MakeRecordBatch*>,
                             public TestFeatherBase {
 public:
  void SetUp() { TestFeatherBase::SetUp(); }

  WriteProperties GetProperties() {
    auto props = WriteProperties::Defaults();
    props.version = kFeatherV2Version;

    // Don't fail if the build doesn't have LZ4_FRAME or ZSTD enabled
    if (!util::Codec::IsAvailable(props.compression)) {
      props.compression = Compression::UNCOMPRESSED;
    }
    return props;
  }
};

TEST(TestFeatherWriteProperties, Defaults) {
  auto props = WriteProperties::Defaults();

#ifdef ARROW_WITH_LZ4
  ASSERT_EQ(Compression::LZ4_FRAME, props.compression);
#else
  ASSERT_EQ(Compression::UNCOMPRESSED, props.compression);
#endif
}

TEST_P(TestFeather, ReadIndicesOrNames) {
  std::shared_ptr<RecordBatch> batch1;
  ASSERT_OK(ipc::test::MakeIntRecordBatch(&batch1));

  ASSERT_OK_AND_ASSIGN(auto table, Table::FromRecordBatches({batch1}));

  DoWrite(*table);

  // int32 type is at the column f4 of the result of MakeIntRecordBatch
  auto expected = Table::Make(schema({field("f4", int32())}), {batch1->column(4)});

  std::shared_ptr<Table> result1, result2;

  std::vector<int> indices = {4};
  ASSERT_OK(reader_->Read(indices, &result1));
  AssertTablesEqual(*expected, *result1);

  std::vector<std::string> names = {"f4"};
  ASSERT_OK(reader_->Read(names, &result2));
  AssertTablesEqual(*expected, *result2);
}

TEST_P(TestFeather, EmptyTable) {
  std::vector<std::shared_ptr<ChunkedArray>> columns;
  auto table = Table::Make(schema({}), columns, 0);

  DoWrite(*table);

  std::shared_ptr<Table> result;
  ASSERT_OK(reader_->Read(&result));
  AssertTablesEqual(*table, *result);
}

TEST_P(TestFeather, SetNumRows) {
  std::vector<std::shared_ptr<ChunkedArray>> columns;
  auto table = Table::Make(schema({}), columns, 1000);
  DoWrite(*table);
  std::shared_ptr<Table> result;
  ASSERT_OK(reader_->Read(&result));
  ASSERT_EQ(1000, result->num_rows());
}

TEST_P(TestFeather, PrimitiveIntRoundTrip) {
  std::shared_ptr<RecordBatch> batch;
  ASSERT_OK(ipc::test::MakeIntRecordBatch(&batch));
  CheckRoundtrip(batch);
}

TEST_P(TestFeather, PrimitiveFloatRoundTrip) {
  std::shared_ptr<RecordBatch> batch;
  ASSERT_OK(ipc::test::MakeFloat3264Batch(&batch));
  CheckRoundtrip(batch);
}

TEST_P(TestFeather, CategoryRoundtrip) {
  std::shared_ptr<RecordBatch> batch;
  ASSERT_OK(ipc::test::MakeDictionaryFlat(&batch));
  CheckRoundtrip(batch);
}

TEST_P(TestFeather, TimeTypes) {
  std::vector<bool> is_valid = {true, true, true, false, true, true, true};
  auto f0 = field("f0", date32());
  auto f1 = field("f1", time32(TimeUnit::MILLI));
  auto f2 = field("f2", timestamp(TimeUnit::NANO));
  auto f3 = field("f3", timestamp(TimeUnit::SECOND, "US/Los_Angeles"));
  auto schema = ::arrow::schema({f0, f1, f2, f3});

  std::vector<int64_t> values64_vec = {0, 1, 2, 3, 4, 5, 6};
  std::shared_ptr<Array> values64;
  ArrayFromVector<Int64Type, int64_t>(is_valid, values64_vec, &values64);

  std::vector<int32_t> values32_vec = {10, 11, 12, 13, 14, 15, 16};
  std::shared_ptr<Array> values32;
  ArrayFromVector<Int32Type, int32_t>(is_valid, values32_vec, &values32);

  std::vector<int32_t> date_values_vec = {20, 21, 22, 23, 24, 25, 26};
  std::shared_ptr<Array> date_array;
  ArrayFromVector<Date32Type, int32_t>(is_valid, date_values_vec, &date_array);

  const auto& prim_values64 = checked_cast<const PrimitiveArray&>(*values64);
  BufferVector buffers64 = {prim_values64.null_bitmap(), prim_values64.values()};

  const auto& prim_values32 = checked_cast<const PrimitiveArray&>(*values32);
  BufferVector buffers32 = {prim_values32.null_bitmap(), prim_values32.values()};

  // Push date32 ArrayData
  std::vector<std::shared_ptr<ArrayData>> arrays;
  arrays.push_back(date_array->data());

  // Create time32 ArrayData
  arrays.emplace_back(ArrayData::Make(schema->field(1)->type(), values32->length(),
                                      BufferVector(buffers32), values32->null_count(),
                                      0));

  // Create timestamp ArrayData
  for (int i = 2; i < schema->num_fields(); ++i) {
    arrays.emplace_back(ArrayData::Make(schema->field(i)->type(), values64->length(),
                                        BufferVector(buffers64), values64->null_count(),
                                        0));
  }

  auto batch = RecordBatch::Make(schema, 7, std::move(arrays));
  CheckRoundtrip(batch);
}

TEST_P(TestFeather, VLenPrimitiveRoundTrip) {
  std::shared_ptr<RecordBatch> batch;
  ASSERT_OK(ipc::test::MakeStringTypesRecordBatch(&batch));
  CheckRoundtrip(batch);
}

TEST_P(TestFeather, PrimitiveNullRoundTrip) {
  std::shared_ptr<RecordBatch> batch;
  ASSERT_OK(ipc::test::MakeNullRecordBatch(&batch));

  ASSERT_OK_AND_ASSIGN(auto table, Table::FromRecordBatches({batch}));

  DoWrite(*table);

  std::shared_ptr<Table> result;
  ASSERT_OK(reader_->Read(&result));

  if (GetParam().version == kFeatherV1Version) {
    std::vector<std::shared_ptr<Array>> expected_fields;
    for (int i = 0; i < batch->num_columns(); ++i) {
      ASSERT_EQ(batch->column_name(i), reader_->schema()->field(i)->name());
      StringArray str_values(batch->column(i)->length(), nullptr, nullptr,
                             batch->column(i)->null_bitmap(),
                             batch->column(i)->null_count());
      AssertArraysEqual(str_values, *result->column(i)->chunk(0));
    }
  } else {
    AssertTablesEqual(*table, *result);
  }
}

TEST_P(TestFeather, SliceIntRoundTrip) {
  std::shared_ptr<RecordBatch> batch;
  ASSERT_OK(ipc::test::MakeIntBatchSized(600, &batch));
  CheckSlices(batch);
}

TEST_P(TestFeather, SliceFloatRoundTrip) {
  std::shared_ptr<RecordBatch> batch;
  // Float16 is not supported by FeatherV1
  ASSERT_OK(ipc::test::MakeFloat3264BatchSized(600, &batch));
  CheckSlices(batch);
}

TEST_P(TestFeather, SliceStringsRoundTrip) {
  std::shared_ptr<RecordBatch> batch;
  ASSERT_OK(ipc::test::MakeStringTypesRecordBatch(&batch, /*with_nulls=*/true));
  CheckSlices(batch);
}

TEST_P(TestFeather, SliceBooleanRoundTrip) {
  std::shared_ptr<RecordBatch> batch;
  ASSERT_OK(ipc::test::MakeBooleanBatchSized(600, &batch));
  CheckSlices(batch);
}

INSTANTIATE_TEST_SUITE_P(
    FeatherTests, TestFeather,
    ::testing::Values(TestParam(kFeatherV1Version), TestParam(kFeatherV2Version),
                      TestParam(kFeatherV2Version, Compression::LZ4_FRAME),
                      TestParam(kFeatherV2Version, Compression::ZSTD)));

#define BATCH_CASES()                                                                    \
  ::testing::Values(                                                                     \
      &ipc::test::MakeIntRecordBatch, &ipc::test::MakeBooleanBatch,                      \
      &ipc::test::MakeFloatBatch, &ipc::test::MakeListRecordBatch,                       \
      &ipc::test::MakeNonNullRecordBatch, &ipc::test::MakeDeeplyNestedList,              \
      &ipc::test::MakeStringTypesRecordBatchWithNulls, &ipc::test::MakeStruct,           \
      &ipc::test::MakeUnion, &ipc::test::MakeDictionary, &ipc::test::MakeDictionaryFlat, \
      &ipc::test::MakeNestedDictionary, &ipc::test::MakeDates,                           \
      &ipc::test::MakeTimestamps, &ipc::test::MakeTimes, &ipc::test::MakeFWBinary,       \
      &ipc::test::MakeNull, &ipc::test::MakeDecimal, &ipc::test::MakeIntervals)

TEST_P(TestFeatherRoundTrip, RoundTrip) {
  std::shared_ptr<RecordBatch> batch;
  ASSERT_OK((*GetParam())(&batch));  // NOLINT clang-tidy gtest issue

  CheckRoundtrip(batch);
}

INSTANTIATE_TEST_SUITE_P(FeatherRoundTripTests, TestFeatherRoundTrip, BATCH_CASES());

}  // namespace feather
}  // namespace ipc
}  // namespace arrow

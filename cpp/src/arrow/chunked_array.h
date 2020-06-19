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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "arrow/result.h"
#include "arrow/status.h"
#include "arrow/type_fwd.h"
#include "arrow/util/macros.h"
#include "arrow/util/visibility.h"

namespace arrow {

class Array;
class DataType;
class MemoryPool;

/// \class ChunkedArray
/// \brief A data structure managing a list of primitive Arrow arrays logically
/// as one large array
class ARROW_EXPORT ChunkedArray {
 public:
  /// \brief Construct a chunked array from a vector of arrays
  ///
  /// The vector must be non-empty and all its elements must have the same
  /// data type.
  explicit ChunkedArray(ArrayVector chunks);

  /// \brief Construct a chunked array from a single Array
  explicit ChunkedArray(std::shared_ptr<Array> chunk)
      : ChunkedArray(ArrayVector{std::move(chunk)}) {}

  /// \brief Construct a chunked array from a vector of arrays and a data type
  ///
  /// As the data type is passed explicitly, the vector may be empty.
  ChunkedArray(ArrayVector chunks, std::shared_ptr<DataType> type);

  /// \return the total length of the chunked array; computed on construction
  int64_t length() const { return length_; }

  /// \return the total number of nulls among all chunks
  int64_t null_count() const { return null_count_; }

  int num_chunks() const { return static_cast<int>(chunks_.size()); }

  /// \return chunk a particular chunk from the chunked array
  std::shared_ptr<Array> chunk(int i) const { return chunks_[i]; }

  const ArrayVector& chunks() const { return chunks_; }

  /// \brief Construct a zero-copy slice of the chunked array with the
  /// indicated offset and length
  ///
  /// \param[in] offset the position of the first element in the constructed
  /// slice
  /// \param[in] length the length of the slice. If there are not enough
  /// elements in the chunked array, the length will be adjusted accordingly
  ///
  /// \return a new object wrapped in std::shared_ptr<ChunkedArray>
  std::shared_ptr<ChunkedArray> Slice(int64_t offset, int64_t length) const;

  /// \brief Slice from offset until end of the chunked array
  std::shared_ptr<ChunkedArray> Slice(int64_t offset) const;

  /// \brief Flatten this chunked array as a vector of chunked arrays, one
  /// for each struct field
  ///
  /// \param[in] pool The pool for buffer allocations, if any
  Result<std::vector<std::shared_ptr<ChunkedArray>>> Flatten(
      MemoryPool* pool = default_memory_pool()) const;

  /// Construct a zero-copy view of this chunked array with the given
  /// type. Calls Array::View on each constituent chunk. Always succeeds if
  /// there are zero chunks
  Result<std::shared_ptr<ChunkedArray>> View(const std::shared_ptr<DataType>& type) const;

  std::shared_ptr<DataType> type() const { return type_; }

  /// \brief Determine if two chunked arrays are equal.
  ///
  /// Two chunked arrays can be equal only if they have equal datatypes.
  /// However, they may be equal even if they have different chunkings.
  bool Equals(const ChunkedArray& other) const;
  /// \brief Determine if two chunked arrays are equal.
  bool Equals(const std::shared_ptr<ChunkedArray>& other) const;

  /// \return PrettyPrint representation suitable for debugging
  std::string ToString() const;

  /// \brief Perform cheap validation checks to determine obvious inconsistencies
  /// within the chunk array's internal data.
  ///
  /// This is O(k*m) where k is the number of array descendents,
  /// and m is the number of chunks.
  ///
  /// \return Status
  Status Validate() const;

  /// \brief Perform extensive validation checks to determine inconsistencies
  /// within the chunk array's internal data.
  ///
  /// This is O(k*n) where k is the number of array descendents,
  /// and n is the length in elements.
  ///
  /// \return Status
  Status ValidateFull() const;

 protected:
  ArrayVector chunks_;
  int64_t length_;
  int64_t null_count_;
  std::shared_ptr<DataType> type_;

 private:
  ARROW_DISALLOW_COPY_AND_ASSIGN(ChunkedArray);
};

namespace internal {

/// \brief EXPERIMENTAL: Utility for incremental iteration over contiguous
/// pieces of potentially differently-chunked ChunkedArray objects
class ARROW_EXPORT MultipleChunkIterator {
 public:
  MultipleChunkIterator(const ChunkedArray& left, const ChunkedArray& right)
      : left_(left),
        right_(right),
        pos_(0),
        length_(left.length()),
        chunk_idx_left_(0),
        chunk_idx_right_(0),
        chunk_pos_left_(0),
        chunk_pos_right_(0) {}

  bool Next(std::shared_ptr<Array>* next_left, std::shared_ptr<Array>* next_right);

  int64_t position() const { return pos_; }

 private:
  const ChunkedArray& left_;
  const ChunkedArray& right_;

  // The amount of the entire ChunkedArray consumed
  int64_t pos_;

  // Length of the chunked array(s)
  int64_t length_;

  // Current left chunk
  int chunk_idx_left_;

  // Current right chunk
  int chunk_idx_right_;

  // Offset into the current left chunk
  int64_t chunk_pos_left_;

  // Offset into the current right chunk
  int64_t chunk_pos_right_;
};

/// \brief Evaluate binary function on two ChunkedArray objects having possibly
/// different chunk layouts. The passed binary function / functor should have
/// the following signature.
///
///    Status(const Array&, const Array&, int64_t)
///
/// The third argument is the absolute position relative to the start of each
/// ChunkedArray. The function is executed against each contiguous pair of
/// array segments, slicing if necessary.
///
/// For example, if two arrays have chunk sizes
///
///   left: [10, 10, 20]
///   right: [15, 10, 15]
///
/// Then the following invocations take place (pseudocode)
///
///   func(left.chunk[0][0:10], right.chunk[0][0:10], 0)
///   func(left.chunk[1][0:5], right.chunk[0][10:15], 10)
///   func(left.chunk[1][5:10], right.chunk[1][0:5], 15)
///   func(left.chunk[2][0:5], right.chunk[1][5:10], 20)
///   func(left.chunk[2][5:20], right.chunk[2][:], 25)
template <typename Action>
Status ApplyBinaryChunked(const ChunkedArray& left, const ChunkedArray& right,
                          Action&& action) {
  MultipleChunkIterator iterator(left, right);
  std::shared_ptr<Array> left_piece, right_piece;
  while (iterator.Next(&left_piece, &right_piece)) {
    ARROW_RETURN_NOT_OK(action(*left_piece, *right_piece, iterator.position()));
  }
  return Status::OK();
}

}  // namespace internal
}  // namespace arrow

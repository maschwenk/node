// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/parsing/literal-buffer.h"

#include <algorithm>

#include "src/base/strings.h"
#include "src/execution/isolate.h"
#include "src/execution/local-isolate.h"
#include "src/heap/factory.h"
#include "src/objects/string-inl.h"
#include "src/utils/memcopy.h"

namespace v8 {
namespace internal {

template <typename IsolateT>
DirectHandle<String> LiteralBuffer::Internalize(IsolateT* isolate) const {
  if (is_one_byte()) {
    return isolate->factory()->InternalizeString(one_byte_literal());
  }
  return isolate->factory()->InternalizeString(two_byte_literal());
}

template DirectHandle<String> LiteralBuffer::Internalize(
    Isolate* isolate) const;
template DirectHandle<String> LiteralBuffer::Internalize(
    LocalIsolate* isolate) const;

template <typename IsolateT>
DirectHandle<String> LiteralBuffer::NewString(IsolateT* isolate) const {
  // Old space, since a LocalIsolate cannot allocate anywhere else.
  if (is_one_byte()) {
    return isolate->factory()
        ->NewStringFromOneByte(one_byte_literal(), AllocationType::kOld)
        .ToHandleChecked();
  }
  base::Vector<const uint16_t> chars = two_byte_literal();
  Handle<SeqTwoByteString> result =
      isolate->factory()
          ->NewRawTwoByteString(chars.length(), AllocationType::kOld)
          .ToHandleChecked();
  DisallowGarbageCollection no_gc;
  // {result} is freshly allocated, so it can't be accessed concurrently.
  CopyChars(
      result->GetChars(no_gc, SharedStringAccessGuardIfNeeded::NotNeeded()),
      chars.begin(), chars.length());
  return result;
}

template DirectHandle<String> LiteralBuffer::NewString(Isolate* isolate) const;
template DirectHandle<String> LiteralBuffer::NewString(
    LocalIsolate* isolate) const;

void LiteralBuffer::AddChars(base::Vector<const uint16_t> code_units) {
  const uint16_t* begin = code_units.begin();
  const uint16_t* end = code_units.end();
  if (is_one_byte()) {
    const uint16_t* first_two_byte = std::find_if(begin, end, [](uint16_t c) {
      return c > unibrow::Latin1::kMaxChar;
    });
    int count = static_cast<int>(first_two_byte - begin);
    if (count > 0) {
      EnsureCapacity(position_ + count);
      CopyChars(backing_store_.begin() + position_, begin, count);
      position_ += count;
    }
    if (first_two_byte == end) return;
    ConvertToTwoByte();
    begin = first_two_byte;
  }
  int size = static_cast<int>(end - begin) * base::kUC16Size;
  EnsureCapacity(position_ + size);
  MemCopy(backing_store_.begin() + position_, begin, size);
  position_ += size;
}

int LiteralBuffer::NewCapacity(int min_capacity) {
  return min_capacity < (kMaxGrowth / (kGrowthFactor - 1))
             ? min_capacity * kGrowthFactor
             : min_capacity + kMaxGrowth;
}

void LiteralBuffer::ExpandBuffer() {
  int min_capacity = std::max({kInitialCapacity, backing_store_.length()});
  ResizeBuffer(NewCapacity(min_capacity));
}

void LiteralBuffer::EnsureCapacity(int min_capacity) {
  if (min_capacity > backing_store_.length()) {
    ResizeBuffer(NewCapacity(std::max(kInitialCapacity, min_capacity)));
  }
}

void LiteralBuffer::ResizeBuffer(int capacity) {
  DCHECK_GE(capacity, position_);
  base::Vector<uint8_t> new_store = base::Vector<uint8_t>::New(capacity);
  if (position_ > 0) {
    MemCopy(new_store.begin(), backing_store_.begin(), position_);
  }
  backing_store_.Dispose();
  backing_store_ = new_store;
}

void LiteralBuffer::ConvertToTwoByte() {
  DCHECK(is_one_byte());
  base::Vector<uint8_t> new_store;
  int new_content_size = position_ * base::kUC16Size;
  if (new_content_size >= backing_store_.length()) {
    // Ensure room for all currently read code units as UC16 as well
    // as the code unit about to be stored.
    new_store = base::Vector<uint8_t>::New(NewCapacity(new_content_size));
  } else {
    new_store = backing_store_;
  }
  uint8_t* src = backing_store_.begin();
  uint16_t* dst = reinterpret_cast<uint16_t*>(new_store.begin());
  for (int i = position_ - 1; i >= 0; i--) {
    dst[i] = src[i];
  }
  if (new_store.begin() != backing_store_.begin()) {
    backing_store_.Dispose();
    backing_store_ = new_store;
  }
  position_ = new_content_size;
  is_one_byte_ = false;
}

void LiteralBuffer::AddTwoByteChar(base::uc32 code_unit) {
  DCHECK(!is_one_byte());
  if (position_ >= backing_store_.length()) ExpandBuffer();
  if (code_unit <=
      static_cast<base::uc32>(unibrow::Utf16::kMaxNonSurrogateCharCode)) {
    *reinterpret_cast<uint16_t*>(&backing_store_[position_]) = code_unit;
    position_ += base::kUC16Size;
  } else {
    *reinterpret_cast<uint16_t*>(&backing_store_[position_]) =
        unibrow::Utf16::LeadSurrogate(code_unit);
    position_ += base::kUC16Size;
    if (position_ >= backing_store_.length()) ExpandBuffer();
    *reinterpret_cast<uint16_t*>(&backing_store_[position_]) =
        unibrow::Utf16::TrailSurrogate(code_unit);
    position_ += base::kUC16Size;
  }
}

}  // namespace internal
}  // namespace v8

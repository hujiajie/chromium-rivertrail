// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/quic_data_reader.h"

using base::StringPiece;

namespace net {

QuicDataReader::QuicDataReader(const char* data, const size_t len)
    : data_(data),
      len_(len),
      pos_(0) {
}

bool QuicDataReader::ReadUInt16(uint16* result) {
  // Make sure that we have the whole uint16.
  // TODO(rch): use sizeof instead of magic numbers.
  // Refactor to use a common Read(void* buffer, size_t len)
  // method that will do the memcpy and the advancement of pos_.
  if (!CanRead(2)) {
    OnFailure();
    return false;
  }

  // Read into result.
  memcpy(result, data_ + pos_, 2);

  // Iterate.
  pos_ += 2;

  return true;
}

bool QuicDataReader::ReadUInt32(uint32* result) {
  // Make sure that we have the whole uint32.
  if (!CanRead(4)) {
    OnFailure();
    return false;
  }

  // Read into result.
  memcpy(result, data_ + pos_, 4);

  // Iterate.
  pos_ += 4;

  return true;
}

bool QuicDataReader::ReadUInt48(uint64* result) {
  uint32 lo;
  if (!ReadUInt32(&lo)) {
    return false;
  }

  uint16 hi;
  if (!ReadUInt16(&hi)) {
    return false;
  }

  *result = hi;
  *result <<= 32;
  *result += lo;

  return true;
}

bool QuicDataReader::ReadUInt64(uint64* result) {
  // Make sure that we have the whole uint64.
  if (!CanRead(8)) {
    OnFailure();
    return false;
  }

  // Read into result.
  memcpy(result, data_ + pos_, 8);

  // Iterate.
  pos_ += 8;

  return true;
}

bool QuicDataReader::ReadUInt128(uint128* result) {
  uint64 high_hash;
  uint64 low_hash;

  if (!ReadUInt64(&low_hash)) {
    return false;
  }
  if (!ReadUInt64(&high_hash)) {
    return false;
  }

  *result = uint128(high_hash, low_hash);
  return true;
}

bool QuicDataReader::ReadStringPiece16(StringPiece* result) {
  // Read resultant length.
  uint16 result_len;
  if (!ReadUInt16(&result_len)) {
    // OnFailure() already called.
    return false;
  }

  return ReadStringPiece(result, result_len);
}

bool QuicDataReader::ReadBytes(void* result, size_t size) {
  // Make sure that we have enough data to read.
  if (!CanRead(size)) {
    OnFailure();
    return false;
  }

  // Read into result.
  memcpy(result, data_ + pos_, size);

  // Iterate.
  pos_ += size;

  return true;
}


bool QuicDataReader::ReadStringPiece(StringPiece* result, size_t size) {
  // Make sure that we have enough data to read.
  if (!CanRead(size)) {
    OnFailure();
    return false;
  }

  // Set result.
  result->set(data_ + pos_, size);

  // Iterate.
  pos_ += size;

  return true;
}

StringPiece QuicDataReader::PeekRemainingPayload() {
  return StringPiece(data_ + pos_, len_ - pos_);
}

StringPiece QuicDataReader::ReadRemainingPayload() {
  StringPiece payload = PeekRemainingPayload();
  pos_ = len_;
  return payload;
}

bool QuicDataReader::IsDoneReading() const {
  return len_ == pos_;
}

size_t QuicDataReader::BytesRemaining() const {
  return len_ - pos_;
}

bool QuicDataReader::CanRead(size_t bytes) const {
  return bytes <= (len_ - pos_);
}

void QuicDataReader::OnFailure() {
  // Set our iterator to the end of the buffer so that further reads fail
  // immediately.
  pos_ = len_;
}

}  // namespace net

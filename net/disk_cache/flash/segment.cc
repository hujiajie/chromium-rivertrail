// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/logging.h"
#include "net/disk_cache/flash/format.h"
#include "net/disk_cache/flash/segment.h"
#include "net/disk_cache/flash/storage.h"

namespace disk_cache {

Segment::Segment(int32 index, bool read_only, Storage* storage)
    : read_only_(read_only),
      init_(false),
      storage_(storage),
      offset_(index * kFlashSegmentSize),
      summary_offset_(offset_ + kFlashSegmentSize - kFlashSummarySize),
      write_offset_(offset_) {
  DCHECK(storage);
  DCHECK(storage->size() % kFlashSegmentSize == 0);
}

Segment::~Segment() {
  DCHECK(!init_ || read_only_);
}

bool Segment::Init() {
  if (init_)
    return false;

  if (offset_ < 0 || offset_ + kFlashSegmentSize > storage_->size())
    return false;

  if (!read_only_) {
    init_ = true;
    return true;
  }

  int32 summary[kFlashMaxEntryCount + 1];
  if (!storage_->Read(summary, kFlashSummarySize, summary_offset_))
    return false;

  size_t entry_count = summary[0];
  DCHECK_LE(entry_count, kFlashMaxEntryCount);

  std::vector<int32> tmp(summary + 1, summary + 1 + entry_count);
  header_offsets_.swap(tmp);
  init_ = true;
  return true;
}

bool Segment::WriteData(const void* buffer, int32 size, int32* offset) {
  DCHECK(init_ && !read_only_);
  DCHECK(CanHold(size));

  if (!storage_->Write(buffer, size, write_offset_))
    return false;
  if (offset)
    *offset = write_offset_;
  write_offset_ += size;
  return true;
}

bool Segment::WriteHeader(const void* header, int32 size, int32* offset) {
  int32 my_offset;
  if (!WriteData(header, size, &my_offset))
    return false;
  if (offset)
    *offset = my_offset;
  header_offsets_.push_back(my_offset);
  return true;
}

bool Segment::ReadData(void* buffer, int32 size, int32 offset) const {
  DCHECK(offset >= offset_ && offset + size <= offset_ + kFlashSegmentSize);
  return storage_->Read(buffer, size, offset);
}

bool Segment::Close() {
  DCHECK(init_);
  if (read_only_)
    return true;

  DCHECK(header_offsets_.size() <= kFlashMaxEntryCount);

  int32 summary[kFlashMaxEntryCount + 1];
  memset(summary, 0, kFlashSummarySize);
  summary[0] = header_offsets_.size();
  std::copy(header_offsets_.begin(), header_offsets_.end(), summary + 1);
  if (!storage_->Write(summary, kFlashSummarySize, summary_offset_))
    return false;

  read_only_ = true;
  return true;
}

bool Segment::CanHold(int32 size) const {
  return header_offsets_.size() < kFlashMaxEntryCount &&
      write_offset_ + size <= summary_offset_;
}

}  // namespace disk_cache

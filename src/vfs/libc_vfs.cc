// Copyright 2017 The BerryDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef _MSC_VER  // Attempt to silence security warnings in MSVC.
#define _CRT_SECURE_NO_WARNINGS
#endif  // _MSC_VER

#include "berrydb/vfs.h"

// TODO(pwnall): This is not as efficient as having separate POSIX and Windows
//               implementations. Write the separate implementations in the
//               nearby future.

#include <cstdio>
#include <tuple>

#include "berrydb/platform.h"
#include "berrydb/status.h"
#include "../util/checks.h"
#include "../util/platform_allocator.h"

namespace berrydb {

namespace {

std::tuple<std::FILE*, size_t> OpenLibcFile(
    const std::string& file_path, bool create_if_missing,
    bool error_if_exists) {
  DCHECK(!error_if_exists || create_if_missing);

  const char* cpath = file_path.c_str();

  FILE* fp;
  if (error_if_exists) {
#if defined(_WIN32) || defined(WIN32)
    fp = std::fopen(cpath, "rb");
    if (fp != nullptr) {
      std::fclose(fp);
      return {nullptr, 0};
    }
    fp = std::fopen(cpath, "wb+");
#else  // defined(_WIN32) || defined(WIN32)
    // NOTE: This relies on C2011.
    fp = std::fopen(cpath, "wb+x");
#endif  // defined(_WIN32) || defined(WIN32)
  } else {
    if (create_if_missing) {
      // NOTE: It might be faster to freopen.
      fp = std::fopen(cpath, "ab+");
      if (fp != nullptr)
        std::fclose(fp);
    }

    fp = std::fopen(cpath, "rb+");
  }

  if (fp != nullptr) {
    if (std::fseek(fp, 0, SEEK_END) == 0)
      return {fp,  std::ftell(fp)};

    // ferror() can be checked if we want to return more detailed errors.
    std::fclose(fp);
  }
  return {nullptr, 0};
}

Status ReadLibcFile(std::FILE* fp, size_t offset, span<uint8_t> buffer) {
  // NOTE(pwnall): On POSIX, we'd want to use pread instead of fseek() and
  //               fread().

  if (std::fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) {
    // ferror() can be checked if we want to return more detailed errors.
    return Status::kIoError;
  }

  if (std::fread(buffer.data(), buffer.size(), 1, fp) != 1) {
    // feof() and ferror() have more details on the error.
    return Status::kIoError;
  }

  return Status::kSuccess;
}

Status WriteLibcFile(std::FILE* fp, span<const uint8_t> data, size_t offset) {
  // NOTE(pwnall): On POSIX, we'd want to use pwrite instead of fseek() and
  //               fwrite().

  if (std::fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) {
    // ferror() can be checked if we want to return more detailed errors.
    return Status::kIoError;
  }

  if (std::fwrite(data.data(), data.size(), 1, fp) != 1) {
    // feof() and ferror() have more details on the error.
    return Status::kIoError;
  }

  return Status::kSuccess;
}

Status SyncLibcFile(std::FILE* fp) {
  // HACK(pwnall): fflush() does not have the guarantees we require, but is
  //               the closest that the C/C++ standard has to offer.
  // This should use fdatasync() on POSIX and FlushFileBuffers() on Windows.
  return (std::fflush(fp) == 0) ? Status::kSuccess : Status::kIoError;
}

}  // namespace

class LibcBlockAccessFile : public BlockAccessFile {
 public:
  LibcBlockAccessFile(FILE* fp, MAYBE_UNUSED size_t block_shift)
      : fp_(fp)
#if BERRYDB_CHECK_IS_ON()
      , block_size_(static_cast<size_t>(1) << block_shift)
#endif  // BERRYDB_CHECK_IS_ON()
      {
    DCHECK(fp != nullptr);

    // Disable buffering, because we're doing block I/O.
    // NOTE(pwnall): This is an incomplete substitute for O_DIRECT.
    std::setbuf(fp, nullptr);
  }

  Status Read(size_t offset, span<uint8_t> buffer) override {
    DCHECK_EQ(buffer.size() & (buffer.size() - 1), 0U);
#if BERRYDB_CHECK_IS_ON()
    DCHECK_EQ(offset & (block_size_ - 1), 0U);
#endif  // BERRYDB_CHECK_IS_ON()

    return ReadLibcFile(fp_, offset, buffer);
  }

  Status Write(span<const uint8_t> data, size_t offset) override {
    DCHECK_EQ(data.size() & (data.size() - 1), 0U);
#if BERRYDB_CHECK_IS_ON()
    DCHECK_EQ(offset & (block_size_ - 1), 0U);
#endif  // BERRYDB_CHECK_IS_ON()

    return WriteLibcFile(fp_, data, offset);
  }

  Status Sync() override { return SyncLibcFile(fp_); }

  Status Lock() override {
    // TODO(pwnall): This should use fcntl(F_SETLK) on POSIX and LockFile() on
    //               Windows. Chromium's File::Lock() implementations are a good
    //               source of inspiration.
    return Status::kSuccess;
  }

  Status Close() override {
    void* heap_block = reinterpret_cast<void*>(this);
    this->~LibcBlockAccessFile();
    Deallocate(heap_block, sizeof(LibcBlockAccessFile));
    return Status::kSuccess;
  }

 protected:
  ~LibcBlockAccessFile() {
    std::fclose(fp_);
  }

 private:
  std::FILE* fp_;

#if BERRYDB_CHECK_IS_ON()
  size_t block_size_;
#endif  // BERRYDB_CHECK_IS_ON()
};

class LibcRandomAccessFile : public RandomAccessFile {
 public:
  LibcRandomAccessFile(FILE* fp) : fp_(fp) {
    DCHECK(fp != nullptr);
  }

  Status Read(size_t offset, span<uint8_t> buffer) override {
    return ReadLibcFile(fp_, offset, buffer);
  }

  Status Write(span<const uint8_t> data, size_t offset) override {
    return WriteLibcFile(fp_, data, offset);
  }

  Status Flush() override {
    return (std::fflush(fp_) == 0) ? Status::kSuccess : Status::kIoError;
  }

  Status Sync() override { return SyncLibcFile(fp_); }

  Status Close() override {
    void* heap_block = reinterpret_cast<void*>(this);
    this->~LibcRandomAccessFile();
    Deallocate(heap_block, sizeof(LibcRandomAccessFile));
    return Status::kSuccess;
  }

 protected:
  ~LibcRandomAccessFile() {
    std::fclose(fp_);
  }

 private:
  std::FILE* fp_;
};

class LibcVfs : public Vfs {
 public:
  std::tuple<Status, RandomAccessFile*, size_t> OpenForRandomAccess(
      const std::string& file_path, bool create_if_missing,
      bool error_if_exists) override {
    FILE* fp;
    size_t file_size;
    std::tie(fp, file_size) = OpenLibcFile(file_path, create_if_missing,
                                           error_if_exists);
    if (fp == nullptr)
      return {Status::kIoError, nullptr, 0};

    void* heap_block = Allocate(sizeof(LibcRandomAccessFile));
    LibcRandomAccessFile* file = new (heap_block) LibcRandomAccessFile(fp);
    DCHECK_EQ(heap_block, reinterpret_cast<void*>(file));
    return {Status::kSuccess, file, file_size};
  }
  std::tuple<Status, BlockAccessFile*, size_t> OpenForBlockAccess(
      const std::string& file_path, size_t block_shift,
      bool create_if_missing, bool error_if_exists) override {
    FILE* fp;
    size_t file_size;
    std::tie(fp, file_size) = OpenLibcFile(file_path, create_if_missing,
                                           error_if_exists);
    if (fp == nullptr)
      return {Status::kIoError, nullptr, 0};

    void* heap_block = Allocate(sizeof(LibcBlockAccessFile));
    LibcBlockAccessFile* file = new (heap_block) LibcBlockAccessFile(
        fp, block_shift);
    DCHECK_EQ(heap_block, reinterpret_cast<void*>(file));
    return {Status::kSuccess, file, file_size};
  }

  Status RemoveFile(const std::string& file_path) override {
    if(std::remove(file_path.c_str()) != 0)
      return Status::kIoError;

    return Status::kSuccess;
  }
};

// TODO(pwnall): Put DefaultVfs() behind an #ifdef, so embedders can use the
//               built-in VFS, but still supply their own default.
Vfs* DefaultVfs() {
  // TODO(pwnall): Check whether this is threadsafe.
  static Vfs* vfs = new (Allocate(sizeof(Vfs))) LibcVfs();

  return vfs;
}

}  // namespace berrydb

// Copyright 2017 The BerryDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BERRYDB_INCLUDE_BERRYDB_VFS_H_
#define BERRYDB_INCLUDE_BERRYDB_VFS_H_

#include <string>
#include <tuple>

#include "berrydb/span.h"
#include "berrydb/types.h"

namespace berrydb {

class BlockAccessFile;
class RandomAccessFile;
enum class Status : int;

/** Pure virtual interface for platform services.
 *
 * The name "Vfs" was chosen because most of the services revolve around file
 * access,
 */
class Vfs {
 public:
  Vfs() noexcept;
  virtual ~Vfs();

  Vfs(const Vfs& other) = delete;
  Vfs(Vfs&& other) = delete;
  Vfs& operator=(const Vfs& other) = delete;
  Vfs& operator=(Vfs&& other) = delete;

  /** Opens a file without any assumptions on the I/O access pattern.
   *
   * This method is used for transaction logs.
   *
   * @param  file_path         the file to be opened or created
   * @param  create_if_missing if true, a new empty file will be created if
   *                           necessary
   * @param  error_if_exists   if true, the call will not succeed if a file
   *                           already exists; create_if_missing must be also
   *                           true if this is true
   * @return Status            attempting to open a non-existing file may result
   *                           in kIoError or kNotFound; all other errors will
   *                           result in kIoError
   * @return RandomAccessFile* used to access the file
   * @return size_t            the number of bytes contained by the file when it
   *                           is opened; the caller is responsible for tracking
   *                           file size changes coming from the caller's code
   */
  virtual std::tuple<Status, RandomAccessFile*, size_t> OpenForRandomAccess(
      const std::string& file_path, bool create_if_missing,
      bool error_if_exists) = 0;

  /** Opens a file designed for reads/writes at (large) block granularities.
   *
   * This method is used for the store data files.
   *
   * @param  file_path         the file to be opened or created
   * @param block_shift        log2(block size); the block size can be computed
   *                           as 1 << block_shift
   * @param  create_if_missing if true, a new empty file will be created if
   *                           necessary
   * @param  error_if_exists   if true, the call will not succeed if a file
   *                           already exists; create_if_missing must be also
   *                           true if this is true
   * @return Status            attempting to open a non-existing file may result
   *                           in kIoError or kNotFound; all other errors will
   *                           result in kIoError
   * @return BlockAccessFile*  used to access the file
   * @return size_t            the number of bytes contained by the file when it
   *                           is opened; the caller is responsible for tracking
   *                           file size changes coming from the caller's code
   */
  virtual std::tuple<Status, BlockAccessFile*, size_t> OpenForBlockAccess(
      const std::string& file_path, size_t block_shift, bool create_if_missing,
      bool error_if_exists) = 0;

  /** Deletes a file from the filesystem.
   *
   * The natural name for this method would have been DeleteFile. However,
   * <windows.h> #defines DeleteFile to DeleteFileW, and it's reasonable to
   * expect this macro to be in effect in parts of our Windows users. RemoveFile
   * is close enough that it's not worth dealing with headaches.
   *
   * @param  file_path the file to be deleted
   * @return           most likely kSuccess or kIoError; attempting to delete a
   *                   non-existing file may result in kNotFound, but that is
   *                   not a requirement
   */
  virtual Status RemoveFile(const std::string& file_path) = 0;
};

/** File I/O interface without any assumptions on the access pattern.
 *
 * This interface is used to access transaction log files.
 *
 * Implementatons are encouraged to use buffering to improve performance. At the
 * same time, any buffering mechanism must obey Flush() and Sync() calls.
 */
class RandomAccessFile {
 public:
  /** Reads a sequence of bytes from the file.
   *
   * @param  offset 0-based file position of the first byte to be read
   * @param  buffer receives the bytes from the file; the span size indicates
   *                the number of bytes to be read
   * @return        most likely kSuccess or kIoError
   */
  virtual Status Read(size_t offset, span<uint8_t> buffer) = 0;

  /** Writes a sequence of bytes to the file.
   *
   * @param  data   the bytes to be written to the file
   * @param  offset 0-based file position of the first byte to be written
   * @return        most likely kSuccess or kIoError
   */
  virtual Status Write(span<const uint8_t> data, size_t offset) = 0;

  /** Evicts any buffered data in the application to the operating system layer.
   *
   * After this method returns successfully, the written data should survive an
   * application crash, as long as the operating system stays alive. Unlike
   * Sync(), the data is not guaranteed to surive a system software crash or
   * a power failure.
   *
   * @return most likely kSuccess or kIoError
   */
  virtual Status Flush() = 0;

  /** Evicts any cached data for the file into persistent storage.
   *
   * After this method returns successfully, the written data should survive a
   * software crash or a power failure. The data may still be lost in the event
   * of a storage medium failure such as a broken charge pump.
   *
   * @return most likely kSuccess or kIoError
   */
  virtual Status Sync() = 0;

  /** Closes the file and releases its underlying resources.
   *
   * This call deallocates the memory used for the RandomAccessFile,
   * invalidating all references to the file.
   */
  virtual Status Close() = 0;

  /** Alias for Close().
   *
   * The alias is introduced so RandomAccessFile behaves like other API objects,
   * which have a Release() method that deallocates their memory.
   */
  inline void Release() { Close(); }

 protected:
  /** Instances must be created using Vfs::OpenForRandomAccess(). */
  RandomAccessFile() noexcept;
  /** Instances must be destroyed using Close(). */
  virtual ~RandomAccessFile();

  // Copy and move assignment + construction are protected so that subclasses
  // can enable them, if they wish to do so.
  RandomAccessFile(const RandomAccessFile& other) noexcept;
  RandomAccessFile(RandomAccessFile&& other) noexcept;
  RandomAccessFile& operator=(const RandomAccessFile& other) noexcept;
  RandomAccessFile& operator=(RandomAccessFile&& other) noexcept;
};

/** Interface for accessing files via block-based I/O.
 *
 * This interface is used for accessing store files. The block size is the store
 * page size.
 *
 * The OpenForBlockAccess() API guarantees that the block size will
 * be a power of two. Implementations are encouraged to take advantage of this
 * guarantee to proxy the I/O calls directly to the operating system, without
 * performing any buffering.
 */
class BlockAccessFile {
 public:
  /** Reads a sequence of blocks from the file.
   *
   * Both the offfset and span size must be multiples of the block size used to
   * open the file.
   *
   * @param  offset 0-based file position of the first byte to be read
   * @param  buffer receives the bytes from the file; the span size indicates
   *                the number of bytes to be read
   * @return        most likely kSuccess or kIoError
   */
  virtual Status Read(size_t offset, span<uint8_t> buffer) = 0;

  /** Writes a sequence of blocks to the file.
   *
   * Both the offfset and span size must be multiples of the block size used to
   * open the file.
   *
   * @param  data   the bytes to be written to the file
   * @param  offset 0-based file position of the first byte to be written
   * @return            most likely kSuccess or kIoError
   */
  virtual Status Write(span<const uint8_t> data, size_t offset) = 0;

  /** Evicts any cached data for the file into persistent storage.
   *
   * After this method returns successfully, the written data should survive a
   * software crash or a power failure. The data may still be lost in the event
   * of a storage medium failure such as a broken charge pump.
   *
   * @return most likely kSuccess or kIoError
   */
  virtual Status Sync() = 0;

  /** Attempts to acquire a mandatory exclusive lock on the file.
   *
   * The file remains locked until it is closed. After this method returns
   * successfully, other processes should not be able to open this file. This
   * process may also not be able to reopen the file, until Close() is called.
   *
   * @return kAlreadyLocked if the lock is already held by another user,
   *         otherwise kSuccess or kIoError
   */
  virtual Status Lock() = 0;

  /** Closes the file and releases its underlying resources.
   *
   * This call deallocates the memory used for the BlockAccessFile, invalidating
   * all references to the file.
   */
  virtual Status Close() = 0;

  /** Alias for Close().
   *
   * The alias is introduced so RandomAccessFile behaves like other API objects,
   * which have a Release() method that deallocates their memory.
   */
  inline void Release() { Close(); }

 protected:
  /** Instances must be created using Vfs::OpenForBlockAccess(). */
  BlockAccessFile() noexcept;
  /** Instances must be destroyed using Close(). */
  virtual ~BlockAccessFile() = 0;

  BlockAccessFile(const BlockAccessFile& other) noexcept;
  BlockAccessFile(BlockAccessFile&& other) noexcept;
  BlockAccessFile& operator=(const BlockAccessFile& other) noexcept;
  BlockAccessFile& operator=(BlockAccessFile&& other) noexcept;
};

/**
 * The VFS associated with resource pools by default.
 *
 * If the vfs/ directory is included, BerryDB will provide a default VFS
 * implementation. Embedders that wish to replace the default should not include
 * vfs/ in their BerryDB build, and should implement berrydb::DefaultVfs().
 */
Vfs* DefaultVfs();

}  // namespace berrydb

#endif  // BERRYDB_INCLUDE_BERRYDB_VFS_H_

// Copyright 2017 The BerryDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "./page.h"

#include <string>

#include "berrydb/options.h"
#include "berrydb/span.h"
#include "berrydb/types.h"
#include "berrydb/vfs.h"
#include "./page_pool.h"
#include "./pool_impl.h"
#include "./store_impl.h"
#include "./test/file_deleter.h"
#include "./util/unique_ptr.h"

#include "gtest/gtest.h"

namespace berrydb {

class PageTest : public ::testing::Test {
 protected:
  PageTest()
      : vfs_(DefaultVfs()), data_file_deleter_(kStoreFileName),
        log_file_deleter_(StoreImpl::LogFilePath(kStoreFileName)) { }

  void SetUp() override {
    Status status;
    BlockAccessFile* raw_data_file;
    std::tie(status, raw_data_file, data_file_size_) = vfs_->OpenForBlockAccess(
        data_file_deleter_.path(), kStorePageShift, true, false);
    ASSERT_EQ(Status::kSuccess, status);
    data_file_.reset(raw_data_file);
    RandomAccessFile* raw_log_file;
    std::tie(status, raw_log_file, log_file_size_) = vfs_->OpenForRandomAccess(
        log_file_deleter_.path(), true, false);
    ASSERT_EQ(Status::kSuccess, status);
    log_file_.reset(raw_log_file);
  }

  void CreatePool(int page_shift, int page_capacity) {
    PoolOptions options;
    options.page_shift = page_shift;
    options.page_pool_size = page_capacity;
    pool_ = PoolImpl::Create(options);
  }

  const std::string kStoreFileName = "test_page.berry";
  constexpr static size_t kStorePageShift = 12;

  Vfs* vfs_;
  FileDeleter data_file_deleter_, log_file_deleter_;
  // Must follow FileDeleter members, because stores must be closed before
  // their files are deleted.
  std::unique_ptr<PoolImpl> pool_;
  UniquePtr<BlockAccessFile> data_file_;
  size_t data_file_size_;
  UniquePtr<RandomAccessFile> log_file_;
  size_t log_file_size_;
};

TEST_F(PageTest, CreateRelease) {
  CreatePool(12, 42);
  PagePool page_pool(pool_.get(), 12, 42);

  Page* page = Page::Create(&page_pool);
  EXPECT_NE(nullptr, page->buffer());
#if DCHECK_IS_ON()
  EXPECT_EQ(nullptr, page->transaction());
  EXPECT_EQ(&page_pool, page->page_pool());
#endif  // DCHECK_IS_ON()

  page->RemovePin();
  EXPECT_TRUE(page->IsUnpinned());

  page->Release(&page_pool);
}

TEST_F(PageTest, Pinning) {
  CreatePool(12, 42);
  PagePool page_pool(pool_.get(), 12, 42);

  Page* page = Page::Create(&page_pool);
  EXPECT_FALSE(page->IsUnpinned());
  page->RemovePin();
  EXPECT_TRUE(page->IsUnpinned());

  page->AddPin();
  EXPECT_FALSE(page->IsUnpinned());

  page->RemovePin();
  EXPECT_TRUE(page->IsUnpinned());

  page->AddPin();
  page->AddPin();
  EXPECT_FALSE(page->IsUnpinned());

  page->RemovePin();
  EXPECT_FALSE(page->IsUnpinned());
  page->RemovePin();
  EXPECT_TRUE(page->IsUnpinned());

  page->Release(&page_pool);
}

TEST_F(PageTest, WillCacheStoreDataDoesNotCacheStoreData) {
  CreatePool(kStorePageShift, 42);
  PagePool* page_pool = pool_->page_pool();
  UniquePtr<StoreImpl> store(StoreImpl::Create(
      data_file_.release(), data_file_size_, log_file_.release(),
      log_file_size_, page_pool, StoreOptions()));

  Page* page = Page::Create(page_pool);
  ASSERT_TRUE(!page->IsUnpinned());

  TransactionImpl* transaction = store->init_transaction();
  page->WillCacheStoreData(transaction, 1337);
  EXPECT_EQ(transaction, page->transaction());
  EXPECT_EQ(1337U, page->page_id());

  page->DoesNotCacheStoreData();
#if DCHECK_IS_ON()
  EXPECT_EQ(nullptr, page->transaction());
#endif  // DCHECK_IS_ON()

  page->Release(page_pool);
}

TEST_F(PageTest, Data) {
  CreatePool(12, 42);
  PagePool page_pool(pool_.get(), 12, 42);

  Page* page = Page::Create(&page_pool);
  EXPECT_FALSE(page->IsUnpinned());

  constexpr const size_t kPageSize = 1 << 12;
  EXPECT_NE(nullptr, page->buffer());
  EXPECT_EQ(page->buffer(), page->mutable_buffer());
  EXPECT_EQ(page->buffer(), page->data(kPageSize).data());
  EXPECT_EQ(kPageSize, page->data(kPageSize).size());
  EXPECT_EQ(page->buffer(), page->mutable_data(kPageSize).data());
  EXPECT_EQ(kPageSize, page->mutable_data(kPageSize).size());

  page->RemovePin();
  EXPECT_TRUE(page->IsUnpinned());
  page->Release(&page_pool);
}

}  // namespace berrydb

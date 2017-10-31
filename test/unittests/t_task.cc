/**
 * This file is part of the CernVM File System.
 */

#include "gtest/gtest.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "atomic.h"
#include "compression.h"
#include "hash.h"
#include "ingestion/item.h"
#include "ingestion/task.h"
#include "ingestion/task_chunk.h"
#include "ingestion/task_compress.h"
#include "ingestion/task_hash.h"
#include "ingestion/task_read.h"
#include "smalloc.h"
#include "util/posix.h"

using namespace std;  // NOLINT

namespace {
class DummyItem {
 public:
  explicit DummyItem(int s) : summand(s) { }
  int summand;
  static atomic_int32 sum;
};
atomic_int32 DummyItem::sum = 0;


class TestTask : public TubeConsumer<DummyItem> {
 public:
  static atomic_int32 cnt_terminate;
  static atomic_int32 cnt_process;
  explicit TestTask(Tube<DummyItem> *tube) : TubeConsumer<DummyItem>(tube) { }

 protected:
  virtual void Process(DummyItem *item) {
    atomic_xadd32(&item->sum, item->summand);
    atomic_inc32(&cnt_process);
  }
  virtual void OnTerminate() { atomic_inc32(&cnt_terminate); }
};
atomic_int32 TestTask::cnt_terminate = 0;
atomic_int32 TestTask::cnt_process = 0;
}


class T_Task : public ::testing::Test {
 protected:
  static const unsigned kNumTasks = 32;

  virtual void SetUp() {
    DummyItem::sum = 0;
    TestTask::cnt_terminate = 0;
    TestTask::cnt_process = 0;
    for (unsigned i = 0; i < kNumTasks; ++i)
      task_group_.TakeConsumer(new TestTask(&tube_));
  }

  virtual void TearDown() {
  }

  Tube<DummyItem> tube_;
  TubeConsumerGroup<DummyItem> task_group_;
};


TEST_F(T_Task, Basic) {
  DummyItem i1(1);
  DummyItem i2(2);
  DummyItem i3(3);

  task_group_.Spawn();
  EXPECT_EQ(0, atomic_read32(&TestTask::cnt_terminate));
  EXPECT_EQ(0, atomic_read32(&TestTask::cnt_process));
  EXPECT_EQ(0, atomic_read32(&DummyItem::sum));

  tube_.Enqueue(&i1);
  tube_.Enqueue(&i2);
  tube_.Enqueue(&i3);

  tube_.Wait();
  task_group_.Terminate();
  EXPECT_EQ(static_cast<int>(kNumTasks),
            atomic_read32(&TestTask::cnt_terminate));

  EXPECT_EQ(6, atomic_read32(&DummyItem::sum));
  EXPECT_EQ(3, atomic_read32(&TestTask::cnt_process));
}


TEST_F(T_Task, Stress) {
  DummyItem i1(1);
  DummyItem i2(2);
  DummyItem i3(3);

  task_group_.Spawn();
  EXPECT_EQ(0, atomic_read32(&TestTask::cnt_terminate));
  EXPECT_EQ(0, atomic_read32(&TestTask::cnt_process));
  EXPECT_EQ(0, atomic_read32(&DummyItem::sum));

  for (unsigned i = 0; i < 10000; ++i) {
    tube_.Enqueue(&i1);
    tube_.Enqueue(&i2);
    tube_.Enqueue(&i3);
  }

  tube_.Wait();
  task_group_.Terminate();
  EXPECT_EQ(static_cast<int>(kNumTasks),
            atomic_read32(&TestTask::cnt_terminate));

  EXPECT_EQ(10000 * 6, atomic_read32(&DummyItem::sum));
  EXPECT_EQ(10000 * 3, atomic_read32(&TestTask::cnt_process));
}


TEST_F(T_Task, Read) {
  Tube<FileItem> tube_in;
  Tube<BlockItem> *tube_out = new Tube<BlockItem>();
  TubeGroup<BlockItem> tube_group_out;
  tube_group_out.TakeTube(tube_out);
  tube_group_out.Activate();

  TubeConsumerGroup<FileItem> task_group;
  task_group.TakeConsumer(new TaskRead(&tube_in, &tube_group_out));
  task_group.Spawn();

  FileItem file_null("/dev/null");
  EXPECT_TRUE(file_null.may_have_chunks());
  tube_in.Enqueue(&file_null);
  BlockItem *item_stop = tube_out->Pop();
  EXPECT_EQ(0U, file_null.size());
  EXPECT_FALSE(file_null.may_have_chunks());
  EXPECT_EQ(BlockItem::kBlockStop, item_stop->type());
  EXPECT_EQ(&file_null, item_stop->file_item());
  delete item_stop;

  string str_abc = "abc";
  EXPECT_TRUE(SafeWriteToFile(str_abc, "./abc", 0600));
  FileItem file_abc("./abc");
  tube_in.Enqueue(&file_abc);
  BlockItem *item_data = tube_out->Pop();
  EXPECT_EQ(3U, file_abc.size());
  EXPECT_EQ(BlockItem::kBlockData, item_data->type());
  EXPECT_EQ(str_abc, string(reinterpret_cast<char *>(item_data->data()),
                            item_data->size()));
  delete item_data;
  item_stop = tube_out->Pop();
  EXPECT_EQ(BlockItem::kBlockStop, item_stop->type());
  delete item_stop;
  unlink("./abc");

  unsigned nblocks = 10;
  int fd_tmp = open("./large", O_CREAT | O_TRUNC | O_WRONLY, 0600);
  EXPECT_GT(fd_tmp, 0);
  for (unsigned i = 0; i < nblocks; ++i) {
    string str_block(TaskRead::kBlockSize, i);
    EXPECT_TRUE(SafeWrite(fd_tmp, str_block.data(), str_block.size()));
  }
  close(fd_tmp);

  unsigned size = nblocks * TaskRead::kBlockSize;
  FileItem file_large("./large", size - 1, size, size + 1);
  tube_in.Enqueue(&file_large);
  for (unsigned i = 0; i < nblocks; ++i) {
    item_data = tube_out->Pop();
    EXPECT_EQ(BlockItem::kBlockData, item_data->type());
    EXPECT_EQ(string(TaskRead::kBlockSize, i),
              string(reinterpret_cast<char *>(item_data->data()),
                                              item_data->size()));
    delete item_data;
  }
  EXPECT_EQ(size, file_large.size());
  EXPECT_TRUE(file_large.may_have_chunks());
  item_stop = tube_out->Pop();
  EXPECT_EQ(BlockItem::kBlockStop, item_stop->type());
  delete item_stop;
  unlink("./large");

  task_group.Terminate();
}


TEST_F(T_Task, ChunkDispatch) {
  Tube<BlockItem> tube_in;
  Tube<BlockItem> *tube_out = new Tube<BlockItem>();
  TubeGroup<BlockItem> tube_group_out;
  tube_group_out.TakeTube(tube_out);
  tube_group_out.Activate();

  TubeConsumerGroup<BlockItem> task_group;
  task_group.TakeConsumer(new TaskChunk(&tube_in, &tube_group_out));
  task_group.Spawn();

  FileItem file_null("/dev/null");
  file_null.set_size(0);
  BlockItem *b1 = new BlockItem(1);
  b1->SetFileItem(&file_null);
  b1->MakeStop();
  tube_in.Enqueue(b1);
  BlockItem *item_stop = tube_out->Pop();
  EXPECT_EQ(0U, tube_out->size());
  EXPECT_EQ(BlockItem::kBlockStop, item_stop->type());
  EXPECT_GE(2 << 28, item_stop->tag());
  EXPECT_EQ(&file_null, item_stop->file_item());
  EXPECT_EQ(&file_null, item_stop->chunk_item()->file_item());
  EXPECT_EQ(0U, item_stop->chunk_item()->size());
  EXPECT_FALSE(item_stop->chunk_item()->is_bulk_chunk());
  delete item_stop->chunk_item();
  delete item_stop;

  file_null.set_may_have_chunks(false);
  BlockItem *b2 = new BlockItem(2);
  b2->SetFileItem(&file_null);
  b2->MakeStop();
  tube_in.Enqueue(b2);
  item_stop = tube_out->Pop();
  EXPECT_EQ(0U, item_stop->chunk_item()->size());
  EXPECT_TRUE(item_stop->chunk_item()->is_bulk_chunk());
  delete item_stop->chunk_item();
  delete item_stop;

  FileItem file_null_legacy("/dev/null", 1024, 2048, 4096,
    zlib::kZlibDefault, shash::kSha1, shash::kSuffixNone, true, true);
  BlockItem *b3 = new BlockItem(3);
  b3->SetFileItem(&file_null_legacy);
  b3->MakeStop();
  tube_in.Enqueue(b3);
  BlockItem *item_stop_chunk = tube_out->Pop();
  EXPECT_FALSE(item_stop_chunk->chunk_item()->is_bulk_chunk());
  delete item_stop_chunk->chunk_item();
  delete item_stop_chunk;
  BlockItem *item_stop_bulk = tube_out->Pop();
  EXPECT_TRUE(item_stop_bulk->chunk_item()->is_bulk_chunk());
  delete item_stop_bulk->chunk_item();
  delete item_stop_bulk;

  task_group.Terminate();
}


TEST_F(T_Task, Chunk) {
  Tube<BlockItem> tube_in;
  Tube<BlockItem> *tube_out = new Tube<BlockItem>();
  TubeGroup<BlockItem> tube_group_out;
  tube_group_out.TakeTube(tube_out);
  tube_group_out.Activate();

  TubeConsumerGroup<BlockItem> task_group;
  task_group.TakeConsumer(new TaskChunk(&tube_in, &tube_group_out));
  task_group.Spawn();

  // Tuned for ~100ms test with many blocks
  unsigned nblocks = 10000;
  unsigned size = nblocks * TaskRead::kBlockSize;
  unsigned avg_chunk_size = 4 * TaskRead::kBlockSize;
  // File does not exist
  FileItem file_large("./large",
                      avg_chunk_size / 2,
                      avg_chunk_size,
                      avg_chunk_size * 2);
  for (unsigned i = 0; i < nblocks; ++i) {
    string str_content(TaskRead::kBlockSize, i);
    unsigned char *content = reinterpret_cast<unsigned char *>(
      smalloc(TaskRead::kBlockSize));
    memcpy(content, str_content.data(), TaskRead::kBlockSize);

    BlockItem *b = new BlockItem(1);
    b->SetFileItem(&file_large);
    b->MakeData(content, TaskRead::kBlockSize);
    tube_in.Enqueue(b);
  }
  BlockItem *b_stop = new BlockItem(1);
  b_stop->SetFileItem(&file_large);
  b_stop->MakeStop();
  tube_in.Enqueue(b_stop);

  unsigned consumed = 0;
  unsigned chunk_size = 0;
  int64_t tag = -1;
  uint64_t last_offset = 0;
  while (consumed < size) {
    BlockItem *b = tube_out->Pop();
    EXPECT_FALSE(b->chunk_item()->is_bulk_chunk());
    if (tag == -1) {
      tag = b->tag();
    } else {
      EXPECT_EQ(tag, b->tag());
    }

    if (b->size() == 0) {
      EXPECT_EQ(BlockItem::kBlockStop, b->type());
      EXPECT_GE(chunk_size, avg_chunk_size / 2);
      EXPECT_LE(chunk_size, avg_chunk_size * 2);
      EXPECT_EQ(consumed, last_offset + chunk_size);
      EXPECT_EQ(chunk_size, b->chunk_item()->size());
      chunk_size = 0;
      tag = -1;
      delete b->chunk_item();
    } else {
      EXPECT_EQ(BlockItem::kBlockData, b->type());
      chunk_size += b->size();
      last_offset = b->chunk_item()->offset();
    }

    consumed += b->size();
    delete b;
  }
  EXPECT_EQ(size, consumed);

  task_group.Terminate();
}


TEST_F(T_Task, CompressNull) {
  Tube<BlockItem> tube_in;
  Tube<BlockItem> *tube_out = new Tube<BlockItem>();
  TubeGroup<BlockItem> tube_group_out;
  tube_group_out.TakeTube(tube_out);
  tube_group_out.Activate();

  TubeConsumerGroup<BlockItem> task_group;
  task_group.TakeConsumer(new TaskCompress(&tube_in, &tube_group_out));
  task_group.Spawn();

  FileItem file_null("/dev/null");
  ChunkItem chunk_null(&file_null, 0);
  BlockItem *b1 = new BlockItem(1);
  b1->SetFileItem(&file_null);
  b1->SetChunkItem(&chunk_null);
  b1->MakeStop();
  tube_in.Enqueue(b1);

  void *ptr_zlib_null;
  uint64_t sz_zlib_null;
  EXPECT_TRUE(zlib::CompressMem2Mem(NULL, 0, &ptr_zlib_null, &sz_zlib_null));

  BlockItem *item_data = tube_out->Pop();
  EXPECT_EQ(BlockItem::kBlockData, item_data->type());
  EXPECT_EQ(sz_zlib_null, item_data->size());
  EXPECT_EQ(0, memcmp(item_data->data(), ptr_zlib_null, sz_zlib_null));
  free(ptr_zlib_null);
  EXPECT_EQ(1, item_data->tag());
  EXPECT_EQ(&file_null, item_data->file_item());
  EXPECT_EQ(&chunk_null, item_data->chunk_item());
  delete item_data;
  BlockItem *item_stop = tube_out->Pop();
  EXPECT_EQ(BlockItem::kBlockStop, item_stop->type());
  EXPECT_EQ(1, item_stop->tag());
  EXPECT_EQ(&file_null, item_stop->file_item());
  EXPECT_EQ(&chunk_null, item_stop->chunk_item());
  delete item_stop;
  EXPECT_EQ(0U, tube_out->size());

  task_group.Terminate();
}


TEST_F(T_Task, Compress) {
  Tube<BlockItem> tube_in;
  Tube<BlockItem> *tube_out = new Tube<BlockItem>();
  TubeGroup<BlockItem> tube_group_out;
  tube_group_out.TakeTube(tube_out);
  tube_group_out.Activate();

  TubeConsumerGroup<BlockItem> task_group;
  task_group.TakeConsumer(new TaskCompress(&tube_in, &tube_group_out));
  task_group.Spawn();

  unsigned size = 16 * 1024 * 1024;
  unsigned block_size = 32 * 1024;
  unsigned nblocks = size / block_size;
  EXPECT_EQ(0U, size % block_size);
  BlockItem block_raw(42);
  block_raw.MakeData(size);
  unsigned char *buf = reinterpret_cast<unsigned char *>(smalloc(size));
  // File does not exist
  FileItem file_large("./large");
  ChunkItem chunk_large(&file_large, 0);
  for (unsigned i = 0; i < nblocks; ++i) {
    string str_content(block_size, i);
    for (unsigned j = 1; j < block_size; ++j)
      str_content[j] = i * str_content[j-1] + j;
    unsigned char *content = reinterpret_cast<unsigned char *>(
      smalloc(block_size));
    memcpy(content, str_content.data(), block_size);

    BlockItem *b = new BlockItem(1);
    b->SetFileItem(&file_large);
    b->SetChunkItem(&chunk_large);
    b->MakeData(content, block_size);
    EXPECT_EQ(block_size, block_raw.Write(b->data(), b->size()));
    tube_in.Enqueue(b);
  }
  EXPECT_EQ(size, block_raw.size());
  BlockItem *b_stop = new BlockItem(1);
  b_stop->SetFileItem(&file_large);
  b_stop->SetChunkItem(&chunk_large);
  b_stop->MakeStop();
  tube_in.Enqueue(b_stop);

  void *ptr_zlib_large = NULL;
  uint64_t sz_zlib_large = 0;
  EXPECT_TRUE(zlib::CompressMem2Mem(
    block_raw.data(), block_raw.size(),
    &ptr_zlib_large, &sz_zlib_large));
  delete buf;

  unsigned char *ptr_read_large = reinterpret_cast<unsigned char *>(
    smalloc(sz_zlib_large));
  unsigned read_pos = 0;

  BlockItem *b = NULL;
  do {
    delete b;
    b = tube_out->Pop();
    EXPECT_EQ(1, b->tag());
    EXPECT_EQ(&file_large, b->file_item());
    EXPECT_EQ(&chunk_large, b->chunk_item());
    EXPECT_LE(read_pos + b->size(), sz_zlib_large);
    if (b->size() > 0) {
      memcpy(ptr_read_large + read_pos, b->data(), b->size());
      read_pos += b->size();
    }
  } while (b->type() == BlockItem::kBlockData);
  EXPECT_EQ(BlockItem::kBlockStop, b->type());
  delete b;
  EXPECT_EQ(0U, tube_out->size());

  EXPECT_EQ(sz_zlib_large, read_pos);
  EXPECT_EQ(0, memcmp(ptr_zlib_large, ptr_read_large, sz_zlib_large));

  free(ptr_read_large);
  free(ptr_zlib_large);
  task_group.Terminate();
}


TEST_F(T_Task, Hash) {
  Tube<BlockItem> tube_in;
  Tube<BlockItem> *tube_out = new Tube<BlockItem>();
  TubeGroup<BlockItem> tube_group_out;
  tube_group_out.TakeTube(tube_out);
  tube_group_out.Activate();

  TubeConsumerGroup<BlockItem> task_group;
  task_group.TakeConsumer(new TaskHash(&tube_in, &tube_group_out));
  task_group.Spawn();

  FileItem file_null("/dev/null");
  ChunkItem chunk_null(&file_null, 0);
  BlockItem b1(1);
  b1.SetFileItem(&file_null);
  b1.SetChunkItem(&chunk_null);
  b1.MakeStop();
  tube_in.Enqueue(&b1);

  BlockItem *item_stop = tube_out->Pop();
  EXPECT_EQ(&b1, item_stop);
  EXPECT_EQ("da39a3ee5e6b4b0d3255bfef95601890afd80709",
            chunk_null.hash_ptr()->ToString());
  EXPECT_EQ(0U, tube_out->size());

  string str_abc = "abc";
  EXPECT_TRUE(SafeWriteToFile(str_abc, "./abc", 0600));
  FileItem file_abc("./abc");
  ChunkItem chunk_abc(&file_abc, 0);
  BlockItem b2_a(2);
  b2_a.SetFileItem(&file_null);
  b2_a.SetChunkItem(&chunk_abc);
  b2_a.MakeData(const_cast<unsigned char *>(
                  reinterpret_cast<const unsigned char *>(str_abc.data())),
                str_abc.size());
  BlockItem b2_b(2);
  b2_b.SetFileItem(&file_null);
  b2_b.SetChunkItem(&chunk_abc);
  b2_b.MakeStop();
  tube_in.Enqueue(&b2_a);
  tube_in.Enqueue(&b2_b);

  BlockItem *item_data = tube_out->Pop();
  EXPECT_EQ(&b2_a, item_data);
  item_stop = tube_out->Pop();
  EXPECT_EQ(&b2_b, item_stop);
  EXPECT_EQ("a9993e364706816aba3e25717850c26c9cd0d89d",
            chunk_abc.hash_ptr()->ToString());
  EXPECT_EQ(0U, tube_out->size());

  b2_a.Discharge();
  unlink("./abc");

  task_group.Terminate();
}
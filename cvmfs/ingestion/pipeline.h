/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_INGESTION_PIPELINE_H_
#define CVMFS_INGESTION_PIPELINE_H_

#include <string>

#include "compression.h"
#include "hash.h"
#include "ingestion/item.h"
#include "ingestion/task.h"
#include "ingestion/tube.h"
#include "upload_spooler_result.h"
#include "util_concurrency.h"

namespace upload {
class AbstractUploader;
struct SpoolerDefinition;
}

class IngestionPipeline : public Observable<upload::SpoolerResult> {
 public:
  explicit IngestionPipeline(
    upload::AbstractUploader *uploader,
    const upload::SpoolerDefinition &spooler_definition);
  ~IngestionPipeline();

  void Spawn();
  void Process(const std::string &path, bool allow_chunking,
               shash::Suffix hash_suffix = shash::kSuffixNone);
  void WaitFor();

  void OnFileProcessed(const upload::SpoolerResult &spooler_result);

 private:
  static const unsigned kNforkRegister = 1;
  static const unsigned kNforkWrite = 1;
  static const unsigned kNforkHash = 2;
  static const unsigned kNforkCompress = 4;
  static const unsigned kNforkChunk = 1;
  static const unsigned kNforkRead = 8;

  const zlib::Algorithms compression_algorithm_;
  const shash::Algorithms hash_algorithm_;
  const bool generate_legacy_bulk_chunks_;
  const bool chunking_enabled_;
  const size_t minimal_chunk_size_;
  const size_t average_chunk_size_;
  const size_t maximal_chunk_size_;

  bool spawned_;
  upload::AbstractUploader *uploader_;
  Tube<FileItem> tube_input_;
  // TODO(jblomer): a semaphore would be faster!
  Tube<FileItem> tube_counter_;

  TubeConsumerGroup<FileItem> tasks_read_;

  TubeGroup<BlockItem> tubes_chunk_;
  TubeConsumerGroup<BlockItem> tasks_chunk_;

  TubeGroup<BlockItem> tubes_compress_;
  TubeConsumerGroup<BlockItem> tasks_compress_;

  TubeGroup<BlockItem> tubes_hash_;
  TubeConsumerGroup<BlockItem> tasks_hash_;

  TubeGroup<BlockItem> tubes_write_;
  TubeConsumerGroup<BlockItem> tasks_write_;

  TubeGroup<FileItem> tubes_register_;
  TubeConsumerGroup<FileItem> tasks_register_;
};  // class IngestionPipeline

#endif  // CVMFS_INGESTION_PIPELINE_H_
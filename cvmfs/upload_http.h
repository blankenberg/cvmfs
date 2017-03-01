/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_UPLOAD_HTTP_H_
#define CVMFS_UPLOAD_HTTP_H_

#include <atomic.h>

#include <string>

#include "session_context.h"
#include "upload_facility.h"

namespace upload {

struct HttpStreamHandle : public UploadStreamHandle {
  explicit HttpStreamHandle(const CallbackTN* commit_callback);
};

class HttpUploader : public AbstractUploader {
 public:
  struct Config {
    Config() : session_token_file(), api_url() {}
    Config(const std::string& session_token_file, const std::string& api_url)
        : session_token_file(session_token_file), api_url(api_url) {}
    std::string session_token_file;
    std::string api_url;
  };

  static bool WillHandle(const SpoolerDefinition& spooler_definition);

  static bool ParseSpoolerDefinition(
      const SpoolerDefinition& spooler_definition, Config* config);

  explicit HttpUploader(const SpoolerDefinition& spooler_definition);

  virtual ~HttpUploader();

  virtual bool Initialize();

  virtual bool FinalizeSession();

  virtual std::string name() const;

  virtual bool Remove(const std::string& file_to_delete);

  virtual bool Peek(const std::string& path) const;

  virtual bool PlaceBootstrappingShortcut(const shash::Any& object) const;

  virtual unsigned int GetNumberOfErrors() const;

 protected:
  virtual void FileUpload(const std::string& local_path,
                          const std::string& remote_path,
                          const CallbackTN* callback = NULL);

  virtual UploadStreamHandle* InitStreamedUpload(
      const CallbackTN* callback = NULL);

  virtual void StreamedUpload(UploadStreamHandle* handle, CharBuffer* buffer,
                              const CallbackTN* callback);

  virtual void FinalizeStreamedUpload(UploadStreamHandle* handle,
                                      const shash::Any& content_hash);

 private:
  void BumpErrors() const;

  Config config_;
  UniquePtr<SessionContext> session_context_;
  mutable atomic_int32 num_errors_;
};

}  // namespace upload

#endif  // CVMFS_UPLOAD_HTTP_H_

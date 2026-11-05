// Windows named-pipe transport. Knows nothing about the SCS SDK or about what
// the messages mean: it only delivers whole messages and disconnect events.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace roadpilot::input {

class MessageSink {
 public:
  virtual ~MessageSink() = default;
  // Called on the pipe thread for every complete message. A chunk larger than
  // the read buffer is reported with size > 64 and must be treated as invalid.
  virtual void on_message(const std::uint8_t* data, std::size_t size) = 0;
  // Called on the pipe thread when a client disconnects or the connection fails.
  virtual void on_disconnect() = 0;
};

constexpr const char* kDefaultPipeName = "\\\\.\\pipe\\roadpilot-input-v1";

class PipeServer {
 public:
  PipeServer(std::string pipe_name, MessageSink& sink);
  ~PipeServer();  // stops the thread
  PipeServer(const PipeServer&) = delete;
  PipeServer& operator=(const PipeServer&) = delete;

  bool start();
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace roadpilot::input

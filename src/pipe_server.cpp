#include "pipe_server.hpp"

#include <windows.h>

#include <atomic>
#include <thread>

namespace roadpilot::input {

struct PipeServer::Impl {
  std::string name;
  MessageSink& sink;
  HANDLE stop_event = nullptr;
  std::thread thread;
  std::atomic<bool> running{false};

  Impl(std::string n, MessageSink& s) : name(std::move(n)), sink(s) {}

  // Waits for `event` or stop. Returns true if the operation event fired.
  bool wait(HANDLE event) {
    HANDLE handles[2] = {event, stop_event};
    return WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0;
  }

  void serve(HANDLE pipe, HANDLE io_event) {
    OVERLAPPED ov{};
    ov.hEvent = io_event;
    ResetEvent(io_event);
    BOOL ok = ConnectNamedPipe(pipe, &ov);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (err == ERROR_IO_PENDING) {
      if (!wait(io_event)) {
        CancelIoEx(pipe, &ov);
        DWORD ignored;
        GetOverlappedResult(pipe, &ov, &ignored, TRUE);
        return;
      }
    } else if (err != ERROR_PIPE_CONNECTED && err != ERROR_SUCCESS) {
      return;
    }

    std::uint8_t buffer[64];
    for (;;) {
      ov = OVERLAPPED{};
      ov.hEvent = io_event;
      ResetEvent(io_event);
      DWORD got = 0;
      ok = ReadFile(pipe, buffer, sizeof(buffer), &got, &ov);
      err = ok ? ERROR_SUCCESS : GetLastError();
      if (err == ERROR_IO_PENDING) {
        if (!wait(io_event)) {
          CancelIoEx(pipe, &ov);
          GetOverlappedResult(pipe, &ov, &got, TRUE);
          break;
        }
        ok = GetOverlappedResult(pipe, &ov, &got, FALSE);
        err = ok ? ERROR_SUCCESS : GetLastError();
      }
      if (err == ERROR_MORE_DATA) {
        sink.on_message(buffer, sizeof(buffer) + 1);  // oversized -> invalid
        break;                                        // drop the connection
      }
      if (err != ERROR_SUCCESS) break;  // broken pipe / failure
      sink.on_message(buffer, got);
    }
    sink.on_disconnect();
    DisconnectNamedPipe(pipe);
  }

  void run() {
    HANDLE io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (running.load() && io_event) {
      // Inbound only: the client writes, the plugin never writes back.
      HANDLE pipe = CreateNamedPipeA(
          name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
          PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
          0, 256, 0, nullptr);
      if (pipe == INVALID_HANDLE_VALUE) {
        if (WaitForSingleObject(stop_event, 500) == WAIT_OBJECT_0) break;
        continue;
      }
      serve(pipe, io_event);
      CloseHandle(pipe);
    }
    if (io_event) CloseHandle(io_event);
  }
};

PipeServer::PipeServer(std::string pipe_name, MessageSink& sink)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), sink)) {}

PipeServer::~PipeServer() { stop(); }

bool PipeServer::start() {
  if (impl_->running.exchange(true)) return true;
  impl_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!impl_->stop_event) {
    impl_->running = false;
    return false;
  }
  impl_->thread = std::thread([this] { impl_->run(); });
  return true;
}

void PipeServer::stop() {
  if (!impl_->running.exchange(false)) return;
  SetEvent(impl_->stop_event);
  if (impl_->thread.joinable()) impl_->thread.join();
  CloseHandle(impl_->stop_event);
  impl_->stop_event = nullptr;
}

}  // namespace roadpilot::input

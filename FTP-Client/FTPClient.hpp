#pragma once
#include <experimental/net>

#include <deque>
#include <memory>
#include <string>
#include <thread>

namespace net = std::experimental::net;

class FTPClient : public std::enable_shared_from_this<FTPClient> {
 public:
  FTPClient(std::string const& hostIP, uint16_t port);
  virtual ~FTPClient();
  void stop();

 private:
  std::string handleFTPMsg220();  // Welcome to fineFTP Server
  std::string handleFTPMsg331();  // Password
  std::string handleFTPMsg230();  // Login Successful
  std::string handleFTPMsg530();  // fail to log in
  std::string handleFTPMsg426();  // Data transfer aborted
  std::string handleFTPMsg503();  // Please specify username first
  std::string handleFTPMsg250();  // "Working directory changed to " +
  std::string handleFTPMsg227();  // Entering passive mode
  std::string handleFTPMsg350();  // Enter targetname
  void handleFTPMsg(std::string const& msg);
  void sendFTPCmd(std::string const& cmd);
  void startSendingCmds();
  void readFTPMsg();

  net::io_context ioContext_;
  net::ip::tcp::socket socket_;
  net::strand<net::io_context::executor_type> cmdWriteStrand_;
  std::thread onlyThread_;

  std::deque<std::string> cmdOutputQueue_;
  std::string remotePath_;
  int lastMsg_;
};

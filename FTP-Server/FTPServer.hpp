#pragma once
#include <experimental/internet>
#include <experimental/io_context>

#include <thread>

#include "UserDatabase.hpp"

namespace net = std::experimental::net;

class FTPServer {
 public:
  FTPServer(unsigned int nbThreads, uint16_t port);
  virtual ~FTPServer();
  void stop();

  // TODO1 remove when done
  void addUser(std::string const& uname, std::string const& pass);

 private:
  void acceptSession(std::error_code const& error, net::ip::tcp::socket& peer);

  UserDatabase userDb_;

  std::vector<std::thread> threadPool_;
  net::io_context ioContext_;
  net::ip::tcp::acceptor acceptor_;
};

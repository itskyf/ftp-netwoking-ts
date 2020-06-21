#include <iostream>

#include "FTPServer.hpp"
#include "FTPSession.hpp"

FTPServer::FTPServer() : acceptor_(ioContext_) {}

FTPServer::~FTPServer() { stop(); }

void FTPServer::start(unsigned int nbThreads, uint16_t port) {
  net::ip::tcp::endpoint endpoint(net::ip::tcp::v4(), port);

  try {
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(net::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
  } catch (std::system_error& const er) {
    std::cerr << er.what() << std::endl;
  }

  std::cout << "FTP Server created. Listening on port "
            << acceptor_.local_endpoint().port() << std::endl;

  acceptor_.async_accept(&FTPServer::acceptSession);

  for (int i = 0; i < nbThreads; ++i) {
    threadPool_.emplace_back([=] { ioContext_.run(); });
  }
}

void FTPServer::stop() {
  ioContext_.stop();
  for (std::thread& thread : threadPool_) {
    thread.join();
  }
  threadPool_.clear();
}

void FTPServer::acceptSession(std::error_code const& error,
                              net::ip::tcp::socket peer) {
  if (error) {
    std::cerr << "Error accepting session" << error.message() << std::endl;
    return;
  }
  std::cout << "FTP Client connected: "
            << peer.remote_endpoint().address().to_string() << ":"
            << peer.remote_endpoint().port() << std::endl;
  auto newSession = std::make_shared<FTPSession>(ioContext_, peer, userDb_);
  newSession->start();
  acceptor_.async_accept(&FTPServer::acceptSession);
}

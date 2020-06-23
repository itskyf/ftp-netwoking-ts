#include <iostream>
#include <map>

#include "FTPClient.hpp"

FTPClient::FTPClient(std::string const& hostIP, uint16_t port)
    : socket_(ioContext_),
      cmdWriteStrand_(ioContext_.get_executor()),
      onlyThread_([&]() { ioContext_.run(); }) {
  net::ip::tcp::endpoint endpoint(net::ip::make_address(hostIP), port);

  // Set a deadline for the connect operation.
  // deadline_.expires_after(boost::asio::chrono::seconds(60));

  // Start the asynchronous connect operation.
  socket_.async_connect(endpoint, [=](std::error_code const& er) {
    if (er) {
      std::cerr << "Cannot connect" << std::endl;
      return;
      // TODO1 reconnect, close socket?
    }
    std::cout << "Connect success" << std::endl;
    std::string username;
    std::cout << "Enter username: ";
    std::getline(std::cin, username);
    readFTPMsg();
    // TODO1 retry user
  });
}

FTPClient::~FTPClient() { stop(); }

void FTPClient::stop() {
  ioContext_.stop();
  onlyThread_.join();
}

void FTPClient::handleFTPMsg(std::string const& msg) {
  const std::map<int, std::function<std::string()>> msgMap{
      {220, [&]() -> std::string { return handleFTPMsg220(); }},
      {331, [&]() -> std::string { return handleFTPMsg331(); }},
      {230, [&]() -> std::string { return handleFTPMsg230(); }},
      {227, [&]() -> std::string { return handleFTPMsg227(); }},
  };
  // TODO1 receive length < 3
  size_t spaceIdx = msg.find_first_of(' ');
  int ftpMsg = std::stoi(msg.substr(0, 3));

  auto msgIt = msgMap.find(ftpMsg);
  if (std::string para =
          spaceIdx != std::string::npos ? msg.substr(spaceIdx + 1) : "";
      msgIt != msgMap.end()) {
    std::string reply = msgIt->second();
    sendFTPCmd(reply);
    lastMsg_ = ftpMsg;
  } else {
    // sendFTPMsg(FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
    //                    "Unrecognized command"));
  }
}

void FTPClient::sendFTPCmd(std::string const& cmd) {
  net::post(cmdWriteStrand_, [me = shared_from_this(), cmd = cmd + "\r\n"]() {
    bool writeInProgress = !me->cmdOutputQueue_.empty();
    me->cmdOutputQueue_.push_back(cmd);
    if (!writeInProgress) {
      me->startSendingCmds();
    }
  });
}

std::string FTPClient::handleFTPMsg220() {
  // TODO1 Ask create / signup here
}

std::string FTPClient::handleFTPMsg331() {
  // TODO1 Input password qt?
  std::string pass;
  std::cout << "Input password: ";
  std::getline(std::cin, pass);
  return pass;
}

std::string FTPClient::handleFTPMsg230() {
  // Login Successful
  return "PWD";
}

std::string FTPClient::handleFTPMsg227() {
  // Login Successful
}

#include <experimental/buffer>

#include <iostream>

#include "FTPClient.hpp"

static bool isNegative(FTPMsg const& msg) { return msg.first >= 400; }

FTPClient::FTPClient() : msgSocket_(ioContext_), dataSocket_(ioContext_) {}

void FTPClient::connect(std::string const& ip, uint16_t port) {
  net::ip::tcp::endpoint remote_endpoint(net::ip::make_address(ip), port);
  try {
    msgSocket_.connect(remote_endpoint);
  } catch (std::system_error const& er) {
    std::cerr << "Server connection error: " << er.what() << std::endl;
    throw er;
    // TODO1 retry
  }
}

void FTPClient::close() {
  try {
    msgSocket_.shutdown(net::ip::tcp::socket::shutdown_both);
    msgSocket_.close();
    // TODO1 data socket
  } catch (std::system_error const& er) {
    std::cerr << "Close error: " << er.what() << std::endl;
    throw er;
    // TODO1 retry
  }
}

bool FTPClient::signup(std::string const& uname, std::string const& pass) {
  try {
    sendCmd("UADD " + uname);
    FTPMsg reply = readFTPMsg();
    if (reply.first == 331) {
      sendCmd("PASS " + pass);
      reply = readFTPMsg();
    } else if (reply.first == 332) {
      // TONOTDO we don't support ACCT command
    }

    return (isNegative(reply));
  } catch (...) {
    // TODO something
  }
}

bool FTPClient::login(std::string const& uname, std::string const& pass) {
  try {
    sendCmd("USER " + uname);
    FTPMsg reply = readFTPMsg();
    if (reply.first == 331) {
      sendCmd("PASS " + pass);
      reply = readFTPMsg();
    } else if (reply.first == 332) {
      // TONOTDO we don't support ACCT command
    }

    return (isNegative(reply));
  } catch (...) {
    // TODO something
  }
}

FTPMsg FTPClient::readFTPMsg() {
  std::error_code ec;
  std::string line;
  size_t len =
      net::read_until(msgSocket_, net::dynamic_buffer(line), "\r\n", ec);

  if (ec == net::error::eof) {
    /* Ignore eof. */
  } else if (ec) {
    std::cerr << "Receive message error: " << ec.message() << std::endl;
    throw ec;
  }

  int code = std::stoi(line.substr(0, 3));
  std::string msg = line.substr(4, len - 2);
  std::cout << "CLI << " << msg << std::endl;
  return std::make_pair(code, msg);
}

void FTPClient::sendCmd(std::string const& cmd) {
  try {
    net::write(msgSocket_, net::buffer(cmd + "\r\n"));
  } catch (std::system_error const& er) {
    std::cerr << "Send command error: " << er.what() << std::endl;
    throw er;
  }
}

bool FTPClient::upload(const std::string& local_file,
                       const std::string& remote_file) {
  std::ifstream file(local_file, std::ios_base::binary);
  if (!file.is_open()) {
    std::cerr << "Cannot open local file " << std::endl;
    return false;
  }
  try {
    if (resetDataSocket()) {
      return false;
    }
    sendCmd("STOR " + remote_file);
    FTPMsg reply = readFTPMsg();
    if (isNegative(reply)) {
      return false;
    }
    /* Start file transfer. */
    charbuf_ptr buffer(std::make_shared<std::vector<char>>(1 << 20));
    while (!file.eof()) {
      file.read(buffer.data(), buffer.size());
      buffer.resize(static_cast<size_t>(file.gcount()));
      if (file.fail() && !file.eof()) {
        std::cerr << "Read local file error" << std::endl;
        return false;
      }

      data_connection->send(buffer.data(), file.gcount());

      if (file.eof()) break;
    }

    /* Don't keep the data connection. */
    closeDataSocket();
    reply = readFTPMsg();
    return isNegative(reply);
  } catch (const connection_exception& ex) {
    handle_connection_exception(ex);
    return command_result::error;
  }
}

bool FTPClient::pwd() {
  sendCmd("PWD");
  FTPMsg reply = readFTPMsg();
  // TODO1 print in here
  return isNegative(reply);
}

bool FTPClient::ls(std::string const& remoteDir) {
  try {
    if (!resetDataSocket()) {
      return false;
    }
    sendCmd("LIST " + remoteDir);
    FTPMsg reply = readFTPMsg();
    if (isNegative(reply)) {
      return false;
    }
    std::string listDirEntry = dataRecv();
    // TODO1 map folder entry Minh Nguyet lam

    /* Don't keep the data connection. */
    closeDataSocket();
    reply = readFTPMsg();
    return isNegative(reply);
  } catch (const connection_exception& ex) {
    handle_connection_exception(ex);
    return command_result::error;
  }
}

bool FTPClient::download(const std::string& remote_file,
                         const std::string& local_file) {
  std::ofstream file(local_file, std::ios_base::binary);

  if (!file.is_open()) {
    std::cerr << "Cannot open local file " << std::endl;
    return false;
  }

  try {
    auto [result, data_connection] = create_data_connection();

    if (result != command_result::ok) {
      return result;
    }

    sendCmd("RETR " + remote_file);

    FTPMsg reply = readFTPMsg();

    report_reply(reply);

    if (isNegative(reply)) {
      return command_result::not_ok;
    }

    /* Start file transfer. */
    for (;;) {
      size_t size = data_connection->recv(buffer_.data(), buffer_.size());

      if (size == 0) break;

      file.write(buffer_.data(), size);

      if (file.fail()) {
        report_error("Cannot write data to file.");
        return command_result::error;
      }
    }

    /* Don't keep the data connection. */
    data_connection->close();

    reply = readFTPMsg();

    report_reply(reply);

    if (isNegative(reply)) {
      return command_result::not_ok;
    } else {
      return command_result::ok;
    }
  } catch (const connection_exception& ex) {
    handle_connection_exception(ex);
    return command_result::error;
  }
}

bool FTPClient::resetDataSocket() {
  // gui pasv goi port
  sendCmd("PASV");
  FTPMsg reply = readFTPMsg();
  if (isNegative(reply)) {
    std::cerr << "Reset data socket error" << std::endl;
    return false;
  }
  // day la IP
  std::string& receivedMsg(reply.second);
  size_t pos = receivedMsg.find("(") + 1;
  size_t pos_1 = receivedMsg.find(",");
  std::string IP = receivedMsg.substr(pos, pos_1 - pos);

  for (int i = 0; i < 3; ++i) {
    IP += ".";
    pos = pos_1 + 1;
    pos_1 = receivedMsg.find(",", pos_1 + 1, 1);
    IP += receivedMsg.substr(pos, pos_1 - pos);
  }

  // day là port
  pos = pos_1 + 1;
  pos_1 = receivedMsg.find(",", pos_1 + 1, 1);
  uint8_t n1 = std::stoi(receivedMsg.substr(pos, pos_1 - pos));
  pos = pos_1 + 1;
  pos_1 = receivedMsg.find(")", pos_1 + 1, 1);
  uint8_t n2 = std::stoi(receivedMsg.substr(pos, pos_1 - pos));
  uint16_t port = (n1 << 8) | n2;

  try {
    closeDataSocket();
    net::ip::tcp::endpoint remoteDataEndpoint(net::ip::make_address(ip), port);
    dataSocket_.connect(remoteDataEndpoint);
  } catch (std::system_error const& er) {
    std::cerr << "Open data socket error: " << er.what() << std::endl;
    dataSocket_.close();
  }
}

std::string FTPClient::dataRecv() {
  std::string reply;
  std::error_code ec;
  net::read(dataSocket_, net::dynamic_buffer(reply), ec);

  if (ec == net::error::eof) {
    /* Ignore eof. */
  } else if (ec) {
    std::cerr << "Data receive error: " << ec.message() << std::endl;
    throw ec;
  }

  return reply;
}

void FTPClient::closeDataSocket() {
  if (dataSocket_.is_open()) {
    dataSocket_.shutdown(net::ip::tcp::socket::shutdown_both);
    dataSocket_.close();
  }
}

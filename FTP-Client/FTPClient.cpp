#include <experimental/buffer>
#include <iostream>

#include "FTPClient.hpp"

struct Dir_Entry {
  std::string ower, group, size, stringtime, filename;
};

static bool isNegative(FTPMsg const& msg) { return msg.first >= 400; }

static std::list<Dir_Entry> string_to_dir_list(std::string const& data) {
  // <cai dong trong struct> \r\n
  // <another> \r\n
  std::stringstream s(data);
  std::string line;
  std::list<Dir_Entry> list;
  while (!s.eof()) {
    std::getline(s, line);
    if (line.back() == '\r') line.pop_back();
    Dir_Entry mem;
    s >> mem.ower >> mem.group >> mem.size >> mem.stringtime >> mem.filename;
    list.push_back(mem);
  }
  return list;
}

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
    FTPMsg reply = recvFTPMsg();
    if (reply.first == 331) {
      sendCmd("PASS " + pass);
      reply = recvFTPMsg();
    } else if (reply.first == 332) {
      // TONOTDO we don't support ACCT command
    }
    return !isNegative(reply);
  } catch (...) {
    // TODOcatch
  }
}

bool FTPClient::login(std::string const& uname, std::string const& pass) {
  try {
    sendCmd("USER " + uname);
    FTPMsg reply = recvFTPMsg();
    if (reply.first == 331) {
      sendCmd("PASS " + pass);
      reply = recvFTPMsg();
    } else if (reply.first == 332) {
      // TONOTDO we don't support ACCT command
    }

    return !isNegative(reply);
  } catch (...) {
    // TODOcatch
  }
}

FTPMsg FTPClient::recvFTPMsg() {
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

bool FTPClient::upload(std::string const& local_file,
                       std::string const& remote_file) {
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
    FTPMsg reply = recvFTPMsg();
    if (isNegative(reply)) {
      return false;
    }
    /* Start file transfer. */
    charbuf_ptr buffer(std::make_shared<std::vector<char>>(1 << 20));
    while (!file.eof()) {
      file.read(buffer->data(), buffer->size());
      buffer->resize(file.gcount());
      if (file.fail() && !file.eof()) {
        std::cerr << "Read local file error" << std::endl;
        return false;
      }
      sendData(buffer);
    }
    /* Don't keep the data connection. */
    file.close();
    closeDataSocket();
    reply = recvFTPMsg();
    return !isNegative(reply);
  } catch (...) {
    // TODOcatch
  }
}

bool FTPClient::mkdir(std::string const& dirName) {
  try {
    sendCmd("MKD " + dirName);
    FTPMsg reply = recvFTPMsg();
    return !isNegative(reply);
  } catch (...) {
    // TODOcatch
  }
}

bool FTPClient::pwd() {
  sendCmd("PWD");
  FTPMsg reply = recvFTPMsg();
  // TODO1 print in here
  return !isNegative(reply);
}

bool FTPClient::ls(std::string const& remoteDir) {
  try {
    if (!resetDataSocket()) {
      return false;
    }
    sendCmd("LIST " + remoteDir);
    FTPMsg reply = recvFTPMsg();
    if (isNegative(reply)) {
      return false;
    }
    std::string listDirStr = recvListDir();
    std::list<Dir_Entry> listEntry = string_to_dir_list(listDirStr);
    /* Don't keep the data connection. */
    closeDataSocket();
    reply = recvFTPMsg();
    return !isNegative(reply);
  } catch (...) {
    // TODOcatch
  }
}

bool FTPClient::cd(std::string const& remoteDir) {
  try {
    sendCmd("CWD " + remoteDir);
    FTPMsg reply = recvFTPMsg();
    return !isNegative(reply);
  } catch (...) {
    // TODOcatch
  }
}

bool FTPClient::download(std::string const& remote_file,
                         std::string const& local_file) {
  std::ofstream file(local_file, std::ios_base::binary);
  if (!file.is_open()) {
    std::cerr << "Cannot open local file " << std::endl;
    return false;
  }
  try {
    if (resetDataSocket()) {
      return false;
    }
    sendCmd("RETR " + remote_file);
    FTPMsg reply = recvFTPMsg();
    if (isNegative(reply)) {
      return false;
    }

    /* Start file transfer. */
    charbuf_ptr bufPtr = std::make_shared<std::vector<char>>(1 << 20);
    while (size_t size = recvData(bufPtr)) {
      file.write(bufPtr->data(), size);
      if (file.fail()) {
        std::cerr << "Write local file error" << std::endl;
        return false;
      }
    }
    /* Don't keep the data connection. */
    file.close();
    closeDataSocket();
    reply = recvFTPMsg();
    return !isNegative(reply);
  } catch (...) {
    // TODOcatch
  }
}

bool FTPClient::resetDataSocket() {
  // gui pasv goi port
  sendCmd("PASV");
  FTPMsg reply = recvFTPMsg();
  if (isNegative(reply)) {
    std::cerr << "Reset data socket error" << std::endl;
    return false;
  }
  // day la IP
  std::string& receivedMsg(reply.second);
  size_t pos = receivedMsg.find("(") + 1, pos_1 = receivedMsg.find(",");
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
    net::ip::tcp::endpoint svDataEndpoint(net::ip::make_address(IP), port);
    dataSocket_.connect(svDataEndpoint);
  } catch (std::system_error const& er) {
    std::cerr << "Open data socket error: " << er.what() << std::endl;
    dataSocket_.close();
  }
}

std::string FTPClient::recvListDir() {
  std::string reply;
  std::error_code ec;
  net::read(dataSocket_, net::dynamic_buffer(reply), ec);

  if (ec == net::error::eof) {
    /* Ignore eof. */
  } else if (ec) {
    std::cerr << "Receive directories error: " << ec.message() << std::endl;
    throw ec;
  }
  return reply;
}

size_t FTPClient::recvData(charbuf_ptr const& bufPtr) {
  std::error_code ec;
  size_t size = dataSocket_.read_some(net::buffer(*bufPtr, bufPtr->size()), ec);

  if (ec == net::error::eof) {
    /* Ignore eof. */
  } else if (ec) {
    std::cerr << "Receive file data err: " << ec.message() << std::endl;
  }
  return size;
}

void FTPClient::sendData(charbuf_ptr const& data) {
  std::error_code ec;
  net::write(dataSocket_, net::buffer(*data), ec);
  if (ec) {
    std::cerr << "Send data aborted: " << ec.message() << std::endl;
    throw ec;
  }
}

void FTPClient::closeDataSocket() {
  if (dataSocket_.is_open()) {
    dataSocket_.shutdown(net::ip::tcp::socket::shutdown_both);
    dataSocket_.close();
  }
}

bool FTPClient::rmdir(std::string const& directory_name) {
  try {
    sendCmd("RMD " + directory_name);
    FTPMsg reply = recvFTPMsg();

    return !isNegative(reply);
  } catch (...) {
    // TODOcatch
  }
}

bool FTPClient::rm(std::string const& remote_file) {
  try {
    sendCmd("RMD " + remote_file);
    FTPMsg reply = recvFTPMsg();

    return !isNegative(reply);
  } catch (...) {
    // TODOcatch
  }
}
#include <map>
#include <functional>
#include <iostream>
#include <iomanip>
#include <string>

#include "command.hpp"

static void cmdls(FTPClient& client, std::string const& para) {
  auto dirList = client.ls(para.empty() ? " " : para);
  if (dirList.first != false && dirList.second.value().size() > 0) {
    for (auto const& entry : dirList.second.value()) {
      std::cout << entry.filename << (entry.dir ? "*" : "") << std::setw(20)
                << entry.size << std::endl;
    }
  }
}

static void cmdcd(FTPClient& client, std::string const& para) {
  if (!para.empty()) {
    client.cd(para);
  }
}

static void cmdmkdir(FTPClient& client, std::string const& para) {
  if (!para.empty()) {
    client.mkdir(para);
  }
}

static void cmdrm(FTPClient& client, std::string const& para) {
  if (!para.empty()) {
    client.rm(para);
  }
}

static void cmdrmdir(FTPClient& client, std::string const& para) {
  if (!para.empty()) {
    client.rmdir(para);
  }
}

static void cmddown(FTPClient& client, std::string const& fl,
                    std::string const& fr) {
  if (!client.download(fl, fr)) {
    std::cout << "Download file failed" << std::endl;
  }
}

static void cmdup(FTPClient& client, std::string const& fl,
                  std::string const& fr) {
  if (!client.upload(fl, fr)) {
    std::cout << "Upload file failed" << std::endl;
  }
}

static const std::map<std::string, std::function<void(FTPClient&, std::string)>>
    cmdMap{
        {"ls", [&](FTPClient& client,
                   std::string const& para) { cmdls(client, para); }},
        {"cd", [&](FTPClient& client,
                   std::string const& para) { cmdcd(client, para); }},
        {"mkdir", [&](FTPClient& client,
                      std::string const& para) { cmdmkdir(client, para); }},
        {"rm", [&](FTPClient& client,
                   std::string const& para) { cmdrm(client, para); }},
        {"rmdir", [&](FTPClient& client,
                      std::string const& para) { cmdrmdir(client, para); }},
    };

bool processCmd(FTPClient& client, std::string const& cmdLine) {
  std::stringstream ss(cmdLine);
  std::string cmd, para, para2;
  ss >> cmd >> para >> para2;
  auto commandIt = cmdMap.find(cmd);
  if (commandIt != cmdMap.end()) {
    commandIt->second(client, para);
  }
  if (cmd == "up") {
    cmdup(client, para, para2);
  }
  if (cmd == "down") {
    cmddown(client, para, para2);
  }
  if (cmd == "exit") {
    return false;
  }
  return true;
}

void startClient(FTPClient& client) {
  std::string ip, uname = "test", pass = "123";
  uint16_t port = 2121;
  std::cout << "Enter server IP: ";
  std::getline(std::cin, ip);
  if (ip.empty()) {
    ip = "192.168.122.24";
  } else {
    std::cout << "Enter port: ";
    std::cin >> port;
    std::cout << "Enter username: ";
    std::getline(std::cin, uname);
    std::cout << "Enter password: ";
    std::getline(std::cin, pass);
  }
  if (!client.connect(ip, port) || !client.login(uname, pass)) {
    return;
  }
  bool status = true;
  std::string cmdLine;
  while (status) {
    auto cwd = client.pwd();
    if (!cwd.first) {
      continue;
    }
    std::cout << uname << '@' << ip << ' ' << cwd.second << "> ";
    std::getline(std::cin, cmdLine);
    status = processCmd(client, cmdLine);
  }
}

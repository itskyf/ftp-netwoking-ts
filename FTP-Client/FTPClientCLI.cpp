#include <iostream>
#include <string>

#include "FTPClient.hpp"

int main() {
  std::string ip;
  std::cout << "Server IP: ";
  std::getline(std::cin, ip);
  uint16_t port;
  std::cout << "Server port: ";
  std::cin >> port;
  FTPClient client;
  client.connect(ip, port);

  return 0;
}
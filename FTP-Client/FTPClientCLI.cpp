#include <iostream>
#include <string>

#include "FTPClient.hpp"

int main() {
  std::string serverIP;
  std::cout << "Server IP: ";
  std::getline(std::cin, serverIP);
  uint16_t port;
  std::cout << "Server port: ";
  std::cin >> port;
  return 0;
}
#include <iostream>
#include <thread>
#include <string>

#include "FTPServer.hpp"

int main() {
  // Create an FTP Server on port 2121. We use 2121 instead of the default port
  // 21, as your application would need root privileges to open port 21.
  FTPServer server(4, 2121);
  server.addUser("test", "123");

  // Add the well known anonymous user and some normal users. The anonymous user
  // can log in with username "anonyous" or "ftp" and any password. The normal
  // users have to provide their username and password.

  // Start the FTP server with 4 threads. More threads will increase the
  // performance with multiple clients, but don't over-do it.

  // Prevent the application from exiting immediatelly
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return 0;
}

#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "FTPUser.hpp"

class UserDatabase {
 public:
  std::shared_ptr<FTPUser> getUser(std::string const& username,
                                   std::string const& password) const;
  std::shared_ptr<FTPUser> addUser(std::string const& username,
                                   std::string const& password,
                                   fs::path const& localRootPath = "");

 private:
  bool isUsernameAnonymousUser(std::string const& username) const;

  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<FTPUser>> userDb_;
  std::shared_ptr<FTPUser> anonymousUser_;
};

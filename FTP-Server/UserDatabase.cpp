#include <iostream>

#include "UserDatabase.hpp"

std::shared_ptr<FTPUser> UserDatabase::getUser(
    std::string const& username, std::string const& password) const {
  std::lock_guard<decltype(mutex_)> userDb_lock(mutex_);

  if (isUsernameAnonymousUser(username)) {
    return anonymousUser_;
  }
  auto userIt = userDb_.find(username);
  return userIt != userDb_.end() && userIt->second->pass_ == password
             ? userIt->second
             : nullptr;
}

std::shared_ptr<FTPUser> UserDatabase::addUser(std::string const& username,
                                               std::string const& password,
                                               fs::path const& localRootPath) {
  std::lock_guard<decltype(mutex_)> userDb_lock(mutex_);

  if (isUsernameAnonymousUser(username)) {
    if (anonymousUser_) {
      std::cerr << "The username denotes the anonymous user, which is "
                   "already present."
                << std::endl;
      return nullptr;
    } else {
      anonymousUser_ = std::make_shared<FTPUser>(password, localRootPath);
      std::cout << "Successfully added anonymous user." << std::endl;
      return anonymousUser_;
    }
  } else {
    if (auto userIt = userDb_.find(username); userIt == userDb_.end()) {
      auto newAcc = std::make_shared<FTPUser>(password, localRootPath);
      userDb_.emplace(username, newAcc);
      std::cout << "Successfully added user \"" << username << "\"."
                << std::endl;
      return newAcc;
    } else {
      std::cerr << "Username \"" << username << "\" already exists."
                << std::endl;
      return nullptr;
    }
  }
}

bool UserDatabase::isUsernameAnonymousUser(std::string const& username) const {
  return username.empty() || username == "ftp" || username == "anonymous";
}

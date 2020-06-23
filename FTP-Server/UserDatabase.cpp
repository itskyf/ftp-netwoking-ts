#include <iostream>

#include "UserDatabase.hpp"

std::shared_ptr<std::string> UserDatabase::getUser(
    std::string const& username, std::string const& password) const {
  std::lock_guard<decltype(database_mutex_)> database_lock(database_mutex_);

  if (isUsernameAnonymousUser(username)) {
    return anonymous_user_;
  }
  auto user_it = database_.find(username);
  return user_it != database_.end() && user_it->second == password
             ? std::make_shared<std::string>(user_it->first)
             : nullptr;
}

bool UserDatabase::addUser(const std::string& username,
                           const std::string& password) {
  std::lock_guard<decltype(database_mutex_)> database_lock(database_mutex_);

  if (isUsernameAnonymousUser(username)) {
    if (anonymous_user_) {
      std::cerr << "The username denotes the anonymous user, which is "
                   "already present."
                << std::endl;
      return false;
    } else {
      anonymous_user_ = std::make_shared<std::string>(username);
      std::cout << "Successfully added anonymous user." << std::endl;
      return true;
    }
  } else {
    auto user_it = database_.find(username);
    if (user_it == database_.end()) {
      database_.emplace(username, password);
      std::cout << "Successfully added user \"" << username << "\"."
                << std::endl;
      return true;
    } else {
      std::cerr << "Username \"" << username << "\" already exists."
                << std::endl;
      return false;
    }
  }
}

bool UserDatabase::isUsernameAnonymousUser(const std::string& username) const {
  return (username.empty() || username == "ftp" || username == "anonymous");
}

#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

class UserDatabase {
 public:
  std::shared_ptr<std::string> getUser(std::string const& username,
                                       std::string const& password) const;
  bool addUser(const std::string& username, const std::string& password);

 private:
  bool isUsernameAnonymousUser(const std::string& username) const;

  mutable std::mutex database_mutex_;
  std::map<std::string, std::string> database_;
  std::shared_ptr<std::string> anonymous_user_;
};

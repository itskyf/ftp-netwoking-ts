#pragma once
#include <memory>
#include <string>

class UserDatabase {
 public:
  std::shared_ptr<std::string> getUser(std::string const& username,
                                       std::string const& password) const;

 private:
};

#pragma once
#include <mutex>
#include <list>
#include <set>

#include "FTPSession.hpp"
class FTPLoggedUser {
 public:
  std::list<std::string> get_logged_user() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::list<std::string> listUser;
    for (auto const& entry : logged_users_) {
      listUser.push_back(entry->getUserName());
    }
    return listUser;
  }

  void join(session_ptr const& ptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    logged_users_.insert(ptr);
    sendMsgToAllClient(ptr->getUserName() + " logged in");
  }

  void leave(session_ptr const& ptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    logged_users_.erase(ptr);
    sendMsgToAllClient(ptr->getUserName() + " logged out");
  }

 private:
  std::mutex mutex_;
  void sendMsgToAllClient(std::string const& msg) {
    for (auto const& user : logged_users_) user->deliver(msg);
  }
  std::set<session_ptr> logged_users_;
};

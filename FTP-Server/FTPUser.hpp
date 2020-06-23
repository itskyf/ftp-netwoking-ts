#pragma once
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

class FTPUser {
 public:
  FTPUser(std::string const& pass, fs::path const& localRootPath);

  std::string const pass_;
  fs::path const localRootPath_;
};

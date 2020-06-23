#include "FTPUser.hpp"

FTPUser::FTPUser(std::string const& pass, fs::path const& localRootPath)
    : pass_(pass),
      localRootPath_(localRootPath.empty() ? fs::current_path()
                                           : localRootPath) {}

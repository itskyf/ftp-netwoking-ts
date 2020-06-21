#include <iostream>

#include "FTPSession.hpp"

static std::map<fs::path, fs::file_status> dirContent(fs::path const& path) {
  std::map<fs::path, fs::file_status> content;
  for (auto const& entry : fs::directory_iterator(path)) {
    content.emplace(entry.path(), entry.status());
  }
  return content;
}

static std::string permString(fs::perms const& p) {
  std::string perm;
  perm += ((p & fs::perms::owner_read) != fs::perms::none ? "r" : "-");
  perm += ((p & fs::perms::owner_write) != fs::perms::none ? "w" : "-");
  perm += ((p & fs::perms::owner_exec) != fs::perms::none ? "x" : "-");
  perm += ((p & fs::perms::group_read) != fs::perms::none ? "r" : "-");
  perm += ((p & fs::perms::group_write) != fs::perms::none ? "w" : "-");
  perm += ((p & fs::perms::group_exec) != fs::perms::none ? "x" : "-");
  perm += ((p & fs::perms::others_read) != fs::perms::none ? "r" : "-");
  perm += ((p & fs::perms::others_write) != fs::perms::none ? "w" : "-");
  perm += ((p & fs::perms::others_exec) != fs::perms::none ? "x" : "-");
  return perm;
}

fs::path const FTPSession::root_(fs::current_path());

FTPSession::FTPSession(net::io_context& context, net::ip::tcp::socket&& socket,
                       UserDatabase const& userDb)
    : userDb_(userDb),
      context_(context),
      cmdSocket_(std::move(socket)),
      cmdWriteStrand_(context_.get_executor()),
      dataTypeBinary_(false),
      fileRWStrand_(context_.get_executor()),
      dataBufStrand_(context_.get_executor()),
      dataAcceptor_(context_),
      ftpWorkingDir_(root_) /*TODO choose*/ {
  // TODO nothing here?
}

FTPSession::~FTPSession() {
  std::cout << "FTP Session shutting down" << std::endl;
  // TODO completion_handler?
}

void FTPSession::start() {
  try {
    cmdSocket_.set_option(net::ip::tcp::no_delay(true));
  } catch (std::system_error const& er) {
    std::cerr << "Unable to set socket option tcp::no_delay: " << er.what()
              << std::endl;
  }
  sendFTPMsg(FTPMsgs(FTPReplyCode::SERVICE_READY_FOR_NEW_USER,
                     "Welcome to fineFTP Server"));
  readFTPCmd();
}

void FTPSession::sendFTPMsg(FTPMsgs const& msg) {
  // TODO checkit bind_executor
  net::post(cmdWriteStrand_,
            [me = shared_from_this(), &msg = std::as_const(msg)]() {
              bool write_in_progress = !me->cmdOutputQueue_.empty();
              me->cmdOutputQueue_.push_back(msg.str());
              if (!write_in_progress) {
                me->startSendingMsgs();
              }
            });
}

void FTPSession::startSendingMsgs() {
  std::cout << "FTP >> " << cmdOutputQueue_.front() << std::endl;
  net::async_write(
      cmdSocket_, net::buffer(cmdOutputQueue_.front()),
      net::bind_executor(
          cmdWriteStrand_,
          [me = shared_from_this()](std::error_code const& ec,
                                    std::size_t /*bytes_to_transfer*/) {
            if (!ec) {
              me->cmdOutputQueue_.pop_front();
              if (!me->cmdOutputQueue_.empty()) {
                me->startSendingMsgs();
              }
            } else {
              std::cerr << "Command write error: " << ec.message() << std::endl;
            }
          }));
}

void FTPSession::readFTPCmd() {
  net::async_read_until(
      cmdSocket_, net::dynamic_buffer(cmdInputStr_), "\r\n",
      [me = shared_from_this()](std::error_code const& ec, size_t length) {
        if (ec) {
          if (ec != net::error::eof) {
            std::cerr << "async_read_until() error: " << ec.message()
                      << std::endl;
          } else {
            std::cout << "Control connection closed by client" << std::endl;
          }
        } else {
          std::string packetStr(me->cmdInputStr_, 0,
                                length - 2);  // Remove \r\n
          std::cout << "FTP << " << packetStr << std::endl;
          me->handleFTPCmd(packetStr);
        }
      });
}

void FTPSession::handleFTPCmd(std::string const& cmd) {
  const std::map<std::string, std::function<FTPMsgs(std::string)>> cmdMap{
      // TODO check lambda this or shared
      // Access control commands
      {"USER",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdUSER(para);
       }},
      {"PASS",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdPASS(para);
       }},
      {"ACCT",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdACCT(para);
       }},
      {"CWD",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdCWD(para);
       }},
      {"CDUP",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdCDUP(para);
       }},
      {"REIN",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdREIN(para);
       }},
      {"QUIT",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdQUIT(para);
       }},
      // Transfer parameter commands
      {"PORT",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdPORT(para);
       }},
      {"PASV",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdPASV(para);
       }},
      {"TYPE",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdTYPE(para);
       }},
      {"STRU",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdSTRU(para);
       }},
      {"MODE",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdMODE(para);
       }},
      // Ftp service commands
      {"RETR",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdRETR(para);
       }},
      {"STOR",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdSTOR(para);
       }},
      {"STOU",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdSTOU(para);
       }},
      {"APPE",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdAPPE(para);
       }},
      {"ALLO",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdALLO(para);
       }},
      {"REST",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdREST(para);
       }},
      {"RNFR",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdRNFR(para);
       }},
      {"RNTO",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdRNTO(para);
       }},
      {"ABOR",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdABOR(para);
       }},
      {"DELE",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdDELE(para);
       }},
      {"RMD",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdRMD(para);
       }},
      {"MKD",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdMKD(para);
       }},
      {"PWD",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdPWD(para);
       }},
      {"LIST",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdLIST(para);
       }},
      {"NLST",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdNLST(para);
       }},
      {"SITE",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdSITE(para);
       }},
      {"SYST",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdSYST(para);
       }},
      {"STAT",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdSTAT(para);
       }},
      {"HELP",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdHELP(para);
       }},
      {"NOOP",
       [this](std::string const& para) -> FTPMsgs {
         this->handleFTPCmdNOOP(para);
       }},
  };
  size_t spaceIdx = cmd.find_first_of(' ');
  std::string ftpCmd = cmd.substr(0, spaceIdx);
  std::transform(ftpCmd.begin(), ftpCmd.end(), ftpCmd.begin(), ::toupper);

  auto commandIt = cmdMap.find(ftpCmd);
  if (std::string para =
          spaceIdx != std::string::npos ? cmd.substr(spaceIdx + 1) : "";
      commandIt != cmdMap.end()) {
    FTPMsgs reply = commandIt->second(para);
    sendFTPMsg(reply);
    lastCmd_ = ftpCmd;
  } else {
    sendFTPMsg(FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                       "Unrecognized command"));
  }
  if (lastCmd_ == "QUIT") {
    // Close command socket
    net::bind_executor(cmdWriteStrand_,
                       [me = shared_from_this()]() { me->cmdSocket_.close(); });
  } else {
    // Wait for next command
    readFTPCmd();
  }
}

fs::path FTPSession::FTP2LocalPath(fs::path const& ftpPath) const {
  fs::path path = ftpPath.is_absolute() ? root_ / ftpPath.relative_path()
                                        : ftpWorkingDir_ / ftpPath;
  return fs::weakly_canonical(path);
}

FTPMsgs FTPSession::checkPathRenamable(fs::path const& ftpPath) const {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (ftpPath.empty()) {
    return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_PARAMETERS, "Empty path");
  }
  fs::path localPath = FTP2LocalPath(ftpPath);
  try {
    if (fs::exists(localPath)) {
      auto fs = fs::directory_iterator(localPath);
      // No read permission -> throw
      return FTPMsgs(FTPReplyCode::COMMAND_OK, "");
    } else {
      return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "File does not exist");
    }
  } catch (fs::filesystem_error const& er) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "Permission denied");
  }
}

void FTPSession::sendDirListing(
    std::map<fs::path, fs::file_status> const& dirContent) {
  dataAcceptor_.async_accept(
      [&dirContent = std::as_const(dirContent), me = shared_from_this()](
          std::error_code const& ec, net::ip::tcp::socket peer) {
        if (ec) {
          me->sendFTPMsg(
              FTPMsgs(FTPReplyCode::TRANSFER_ABORTED, "Data transfer aborted"));
          return;
        }
        // TODO: close acceptor after connect?
        // Create a Unix-like file list
        std::stringstream stream;
        std::string owner = "hcmus", group = "hcmus";
        for (auto const& entry : dirContent) {
          auto const& filePath(entry.first);
          auto const& fileStatus(entry.second);
          stream << (fs::is_directory(fileStatus) ? 'd' : '-')
                 << permString(fileStatus.permissions()) << "   1 ";
          stream << std::setw(10) << owner << " " << std::setw(10) << group
                 << " " << std::setw(10) << fs::file_size(filePath) << " ";
          // TODO stream << fileStatus.timeString() << " ";
          stream << filePath.filename() << "\r\n";
        }

        // Copy the file list into a raw char vector
        std::string dirListStr = stream.str();
        std::shared_ptr<std::vector<char>> dir_listing_rawdata =
            std::make_shared<std::vector<char>>();
        dir_listing_rawdata->reserve(dirListStr.size());
        std::copy(dirListStr.begin(), dirListStr.end(),
                  std::back_inserter(*dir_listing_rawdata));

        // Send the string out
        me->addDataToBufferAndSend(peer, dir_listing_rawdata);
        me->addDataToBufferAndSend(
            peer,
            std::shared_ptr<std::vector<char>>());  // Nullpointer indicates
                                                    // end of transmission
      });
}

void FTPSession::sendNameList(
    std::map<fs::path, fs::file_status> const& dirContent) {
  dataAcceptor_.async_accept(
      [&dirContent = std::as_const(dirContent), me = shared_from_this()](
          std::error_code const& ec, net::ip::tcp::socket peer) {
        if (ec) {
          me->sendFTPMsg(
              FTPMsgs(FTPReplyCode::TRANSFER_ABORTED, "Data transfer aborted"));
          return;
        }

        // Create a file list
        std::stringstream stream;
        for (const auto& entry : dirContent) {
          stream << entry.first.filename() << "\r\n";
        }

        // Copy the file list into a raw char vector
        std::string dir_listing_string = stream.str();
        std::shared_ptr<std::vector<char>> dir_listing_rawdata =
            std::make_shared<std::vector<char>>();
        dir_listing_rawdata->reserve(dir_listing_string.size());
        std::copy(dir_listing_string.begin(), dir_listing_string.end(),
                  std::back_inserter(*dir_listing_rawdata));

        // Send the string out
        me->addDataToBufferAndSend(peer, dir_listing_rawdata);
        me->addDataToBufferAndSend(
            peer,
            std::shared_ptr<std::vector<char>>());  // Nullpointer indicates end
                                                    // of transmission
      });
}

// FTP Commands
// Access control commands
FTPMsgs FTPSession::handleFTPCmdUSER(std::string const& param) {
  loggedUser_ = nullptr;
  username_ = param;
  ftpWorkingDir_ = root_;
  return param.empty()
             ? FTPMsgs(FTPReplyCode::SYNTAX_ERROR_PARAMETERS,
                       "Please provide username")
             : FTPMsgs(FTPReplyCode::USER_NAME_OK, "Please enter password");
}

FTPMsgs FTPSession::handleFTPCmdPASS(std::string const& param) {
  if (lastCmd_ != "USER") {
    return FTPMsgs(FTPReplyCode::COMMANDS_BAD_SEQUENCE,
                   "Please specify username first");
  }
  if (auto user = userDb_.getUser(username_, param); user) {
    loggedUser_ = user;
    return FTPMsgs(FTPReplyCode::USER_LOGGED_IN, "Login successful");
  } else {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Failed to log in");
  }
}

FTPMsgs FTPSession::handleFTPCmdACCT(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                 "Unsupported command");
}

FTPMsgs FTPSession::handleFTPCmdCWD(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (param.empty()) {
    return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_PARAMETERS, "No path given");
  }

  fs::path absNewWorkingDir = FTP2LocalPath(param);
  // TODO network drive
  try {
    auto status = fs::exists(absNewWorkingDir);
  } catch (fs::filesystem_error const&) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN,
                   "Failed changing directory: The given resource does not "
                   "exist or permission denied.");
  }

  if (!fs::is_directory(absNewWorkingDir)) {
    return FTPMsgs(
        FTPReplyCode::ACTION_NOT_TAKEN,
        "Failed ot change directory: The given resource is not a directory.");
  }

  try {
    auto dirIt = fs::directory_iterator(absNewWorkingDir);
  } catch (fs::filesystem_error const&) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN,
                   "Failed ot change directory: Permission denied.");
  }

  ftpWorkingDir_ = absNewWorkingDir;
  return FTPMsgs(
      FTPReplyCode::FILE_ACTION_COMPLETED,
      "Working directory changed to " + fs::weakly_canonical(param).string());
}

FTPMsgs FTPSession::handleFTPCmdCDUP(std::string const& /*param*/) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (ftpWorkingDir_ != root_) {
    // Only CDUP when we are not already at the root directory
    FTPMsgs cwdReply = handleFTPCmdCWD("..");
    // The CWD returns FILE_ACTION_COMPLETED on success, while CDUP returns
    // COMMAND_OK on success.
    return cwdReply.replyCode() == FTPReplyCode::FILE_ACTION_COMPLETED
               ? FTPMsgs(FTPReplyCode::COMMAND_OK, cwdReply.msg())
               : cwdReply;
  } else {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "Already at root directory");
  }
}

FTPMsgs FTPSession::handleFTPCmdREIN(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::COMMAND_NOT_IMPLEMENTED, "Unsupported command");
}

FTPMsgs FTPSession::handleFTPCmdQUIT(std::string const& /*param*/) {
  loggedUser_ = nullptr;
  return FTPMsgs(FTPReplyCode::SERVICE_CLOSING_CONTROL_CONNECTION,
                 "Connection shutting down");
}

// Transfer parameter commands
FTPMsgs FTPSession::handleFTPCmdPORT(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                 "FTP active mode is not supported by this server");
}

FTPMsgs FTPSession::handleFTPCmdPASV(std::string const& /*param*/) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }

  try {
    if (dataAcceptor_.is_open()) {
      dataAcceptor_.close();
    }
    net::ip::tcp::endpoint endpoint(net::ip::tcp::v4(), 0);
    dataAcceptor_.open(endpoint.protocol());
    dataAcceptor_.bind(endpoint);
    dataAcceptor_.listen(net::socket_base::max_listen_connections);
  } catch (std::system_error& er) {
    std::cerr << er.what() << std::endl;
    return FTPMsgs(FTPReplyCode::SERVICE_NOT_AVAILABLE,
                   "Failed to enter passive mode.");
  }
  // Split address and port into bytes and get the port the OS chose for us
  auto ipBytes = cmdSocket_.local_endpoint().address().to_v4().to_bytes();
  auto port = dataAcceptor_.local_endpoint().port();

  // Form reply string
  std::stringstream stream("(");
  for (int i = 0; i < 4; ++i) {
    stream << static_cast<int>(ipBytes[i]) << ",";
  }
  stream << ((port >> 8) & 0xff) << "," << (port & 0xff) << ")";
  return FTPMsgs(FTPReplyCode::ENTERING_PASSIVE_MODE,
                 "Entering passive mode " + stream.str());
}

FTPMsgs FTPSession::handleFTPCmdTYPE(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (param == "A") {
    dataTypeBinary_ = false;
    // TODO: The ASCII mode currently does not work as RFC 959 demands it. It
    // should perform line ending conversion, which it doesn't. But as we are
    // living in the 21st centry, nobody should use ASCII mode anyways.
    return FTPMsgs(FTPReplyCode::COMMAND_OK, "Switching to ASCII mode");
  } else if (param == "I") {
    dataTypeBinary_ = true;
    return FTPMsgs(FTPReplyCode::COMMAND_OK, "Switching to binary mode");
  } else {
    return FTPMsgs(FTPReplyCode::COMMAND_NOT_IMPLEMENTED_FOR_PARAMETER,
                   "Unknown or unsupported type");
  }
}

FTPMsgs FTPSession::handleFTPCmdSTRU(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                 "Unsupported command");
}

FTPMsgs FTPSession::handleFTPCmdMODE(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                 "Unsupported command");
}

// Ftp service commands
FTPMsgs FTPSession::handleFTPCmdRETR(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }

  if (!dataAcceptor_.is_open()) {
    return FTPMsgs(FTPReplyCode::ERROR_OPENING_DATA_CONNECTION,
                   "Error opening data connection");
  }

  fs::path localPath = FTP2LocalPath(param);
  std::ios::openmode openMode =
      (dataTypeBinary_ ? (std::ios::in | std::ios::binary) : (std::ios::in));
  std::shared_ptr<IoFile> file = std::make_shared<IoFile>(localPath, openMode);
  if (!file->fileStream_.good()) {
    return FTPMsgs(FTPReplyCode::ACTION_ABORTED_LOCAL_ERROR,
                   "Error opening file for transfer");
  }
  sendFile(file);
  return FTPMsgs(FTPReplyCode::FILE_STATUS_OK_OPENING_DATA_CONNECTION,
                 "Sending file");
}

FTPMsgs FTPSession::handleFTPCmdSTOR(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  // TODO: the ACTION_NOT_TAKEN reply is not RCF 959 conform. Apparently in
  // 1985 nobody anticipated that you might not want anybody uploading files
  // to your server. We use the return code anyways, as the popular FileZilla
  // Server also returns that code as "Permission denied"
  if (!dataAcceptor_.is_open()) {
    return FTPMsgs(FTPReplyCode::ERROR_OPENING_DATA_CONNECTION,
                   "Error opening data connection");
  }

  fs::path localPath = FTP2LocalPath(param);
  try {
    if (fs::is_directory(localPath)) {
      return FTPMsgs(
          FTPReplyCode::ACTION_NOT_TAKEN_FILENAME_NOT_ALLOWED,
          "Cannot create file. A directory with that name already exists.");
    }
  } catch (fs::filesystem_error const& er) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN_FILENAME_NOT_ALLOWED,
                   "Cannot read file status.");
  }

  std::ios::openmode openMode =
      (dataTypeBinary_ ? (std::ios::out | std::ios::binary) : (std::ios::out));
  std::shared_ptr<IoFile> file = std::make_shared<IoFile>(localPath, openMode);
  if (!file->fileStream_.good()) {
    return FTPMsgs(FTPReplyCode::ACTION_ABORTED_LOCAL_ERROR,
                   "Error opening file for transfer");
  }
  receiveFile(file);
  return FTPMsgs(FTPReplyCode::FILE_STATUS_OK_OPENING_DATA_CONNECTION,
                 "Receiving file");
}

FTPMsgs FTPSession::handleFTPCmdSTOU(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                 "Command not implemented");
}

FTPMsgs FTPSession::handleFTPCmdAPPE(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (!dataAcceptor_.is_open()) {
    return FTPMsgs(FTPReplyCode::ERROR_OPENING_DATA_CONNECTION,
                   "Error opening data connection");
  }

  fs::path localPath = FTP2LocalPath(param);
  try {
    if (!fs::is_regular_file(localPath)) {
      return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "File does not exist.");
    }
  } catch (fs::filesystem_error const& er) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "Cannot read file status.");
  }

  std::ios::openmode openMode =
      (dataTypeBinary_ ? (std::ios::out | std::ios::app | std::ios::binary)
                       : (std::ios::out | std::ios::app));
  std::shared_ptr<IoFile> file = std::make_shared<IoFile>(localPath, openMode);

  if (!file->fileStream_.good()) {
    return FTPMsgs(FTPReplyCode::ACTION_ABORTED_LOCAL_ERROR,
                   "Error opening file for transfer");
  }
  receiveFile(file);
  return FTPMsgs(FTPReplyCode::FILE_STATUS_OK_OPENING_DATA_CONNECTION,
                 "Receiving file");
}

FTPMsgs FTPSession::handleFTPCmdALLO(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                 "Command not implemented");
}

FTPMsgs FTPSession::handleFTPCmdREST(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::COMMAND_NOT_IMPLEMENTED,
                 "Command not implemented");
}

FTPMsgs FTPSession::handleFTPCmdRNFR(std::string const& param) {
  if (FTPMsgs isRenamableErr = checkPathRenamable(param);
      isRenamableErr.replyCode() == FTPReplyCode::COMMAND_OK) {
    renameSrcPath_ = param;
    return FTPMsgs(FTPReplyCode::FILE_ACTION_NEEDS_FURTHER_INFO,
                   "Enter target name");
  } else {
    return isRenamableErr;
  }
}

FTPMsgs FTPSession::handleFTPCmdRNTO(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (lastCmd_ != "RNFR" || renameSrcPath_.empty()) {
    return FTPMsgs(FTPReplyCode::COMMANDS_BAD_SEQUENCE,
                   "Please specify target file first");
  }
  if (param.empty()) {
    return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_PARAMETERS,
                   "No target name given");
  }

  // TODO: returning neiher FILE_ACTION_NOT_TAKEN nor ACTION_NOT_TAKEN are
  // RFC 959 conform. Aoarently back in 1985 it was assumed that the RNTO
  // command will always succeed, as long as you enter a valid target file
  // name. Thus we use the two return codes anyways, the popular FileZilla
  // FTP Server uses those as well.
  if (FTPMsgs isRenamableErr = checkPathRenamable(renameSrcPath_);
      isRenamableErr.replyCode() == FTPReplyCode::COMMAND_OK) {
    fs::path localSrcPath = FTP2LocalPath(renameSrcPath_),
             localDstPath = FTP2LocalPath(param);
    // Check if the source file exists already. We simple disallow overwriting a
    // file be renaming (the bahavior of the native rename command on Windows
    // and Linux differs; Windows will not overwrite files, Linux will).
    if (fs::exists(localDstPath)) {
      return FTPMsgs(FTPReplyCode::FILE_ACTION_NOT_TAKEN,
                     "Target path exists already.");
    }

    std::error_code ec;
    // TODO catch????
    fs::rename(localSrcPath, localDstPath, ec);
    return ec ? FTPMsgs(FTPReplyCode::FILE_ACTION_NOT_TAKEN,
                        "Error renaming file")
              : FTPMsgs(FTPReplyCode::FILE_ACTION_COMPLETED, "OK");
  } else {
    return isRenamableErr;
  }
}

FTPMsgs FTPSession::handleFTPCmdABOR(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::COMMAND_NOT_IMPLEMENTED,
                 "Command not implemented");
}

FTPMsgs FTPSession::handleFTPCmdDELE(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }

  fs::path localPath = FTP2LocalPath(param);
  if (!fs::exists(localPath)) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "Resource does not exist");
  } else if (!fs::is_regular_file(localPath)) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "Resource is not a file");
  } else {
    return fs::remove(localPath) ? FTPMsgs(FTPReplyCode::FILE_ACTION_COMPLETED,
                                           "Successfully deleted file")
                                 : FTPMsgs(FTPReplyCode::FILE_ACTION_NOT_TAKEN,
                                           "Unable to delete file");
  }
}

FTPMsgs FTPSession::handleFTPCmdRMD(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  fs::path localPath = FTP2LocalPath(param);
  return fs::remove_all(localPath)
             ? FTPMsgs(FTPReplyCode::FILE_ACTION_COMPLETED,
                       "Successfully removed directory")
             : FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN,
                       "Unable to remove directory");
}

FTPMsgs FTPSession::handleFTPCmdMKD(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  fs::path localPath = FTP2LocalPath(param);
  return fs::create_directory(localPath)
             ? FTPMsgs(FTPReplyCode::PATHNAME_CREATED,
                       "Successfully created directory " + localPath.string())
             : FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN,
                       "Unable to create directory");
}

FTPMsgs FTPSession::handleFTPCmdPWD(std::string const& /*param*/) {
  // RFC 959 does not allow returning NOT_LOGGED_IN here, so we abuse
  // ACTION_NOT_TAKEN for that.
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "Not logged in");
  }
  return FTPMsgs(FTPReplyCode::PATHNAME_CREATED, ftpWorkingDir_.string());
}

FTPMsgs FTPSession::handleFTPCmdLIST(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }

  fs::path localPath = FTP2LocalPath(param);
  try {
    if (fs::exists(localPath)) {
      if (fs::is_directory(localPath)) {
        sendDirListing(dirContent(localPath));
        return FTPMsgs(FTPReplyCode::FILE_STATUS_OK_OPENING_DATA_CONNECTION,
                       "Sending directory listing");
      } else {
        // TODO: RFC959: If the pathname specifies a file then the server should
        // send current information on the file.
        return FTPMsgs(FTPReplyCode::FILE_ACTION_NOT_TAKEN,
                       "Path is not a directory");
      }
    } else {
      return FTPMsgs(FTPReplyCode::FILE_ACTION_NOT_TAKEN,
                     "Path does not exist");
    }
  } catch (fs::filesystem_error const&) {
    return FTPMsgs(FTPReplyCode::FILE_ACTION_NOT_TAKEN, "Permission denied");
  }
}

FTPMsgs FTPSession::handleFTPCmdNLST(std::string const& param) {
  if (!loggedUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }

  fs::path localPath = FTP2LocalPath(param);
  try {
    if (fs::exists(localPath)) {
      if (fs::is_directory(localPath)) {
        sendNameList(dirContent(localPath));
        return FTPMsgs(FTPReplyCode::FILE_STATUS_OK_OPENING_DATA_CONNECTION,
                       "Sending name list");
      } else {
        // TODO: RFC959: If the pathname specifies a file then the server should
        // send current information on the file.
        return FTPMsgs(FTPReplyCode::FILE_ACTION_NOT_TAKEN,
                       "Path is not a directory");
      }
    } else {
      return FTPMsgs(FTPReplyCode::FILE_ACTION_NOT_TAKEN,
                     "Path does not exist");
    }
  } catch (fs::filesystem_error const&) {
    return FTPMsgs(FTPReplyCode::FILE_ACTION_NOT_TAKEN, "Permission denied");
  }
}

FTPMsgs FTPSession::handleFTPCmdSITE(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                 "Command not implemented");
}

FTPMsgs FTPSession::handleFTPCmdSYST(std::string const& /*param*/) {
#if defined _WIN32 || defined _WIN64
  return FTPMsgs(FTPReplyCode::NAME_SYSTEM_TYPE, "WIN32");
#elif defined __ANDROID__
  return FTPMsgs(FTPReplyCode::NAME_SYSTEM_TYPE, "LINUX");
#elif defined __linux__
  return FTPMsgs(FTPReplyCode::NAME_SYSTEM_TYPE, "LINUX");
#elif defined __APPLE__ && __MACH__
  return FTPMsgs(FTPReplyCode::NAME_SYSTEM_TYPE, "MACOS");
#elif defined __FreeBSD__
  return FTPMsgs(FTPReplyCode::NAME_SYSTEM_TYPE, "FREEBSD");
#elif defined __NetBSD__
  return FTPMsgs(FTPReplyCode::NAME_SYSTEM_TYPE, "NETBSD");
#elif defined __OpenBSD__
  return FTPMsgs(FTPReplyCode::NAME_SYSTEM_TYPE, "OPENBSD");
#else
  return FTPMsgs(FTPReplyCode::NAME_SYSTEM_TYPE, "UNKNOWN");
#endif
}

FTPMsgs FTPSession::handleFTPCmdSTAT(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::COMMAND_NOT_IMPLEMENTED,
                 "Command not implemented");
}

FTPMsgs FTPSession::handleFTPCmdHELP(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::COMMAND_NOT_IMPLEMENTED,
                 "Command not implemented");
}

FTPMsgs FTPSession::handleFTPCmdNOOP(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::COMMAND_OK, "OK");
}

void FTPSession::sendFile(std::shared_ptr<IoFile> file) {
  dataAcceptor_.async_accept(
      [file, me = shared_from_this()](std::error_code const& ec,
                                      net::ip::tcp::socket peer) {
        if (ec) {
          me->sendFTPMsg(
              FTPMsgs(FTPReplyCode::TRANSFER_ABORTED, "Data transfer aborted"));
          return;
        }
        // Start sending multiple buffers at once
        me->readFileDataAndSend(peer, file);
        me->readFileDataAndSend(peer, file);
        me->readFileDataAndSend(peer, file);
      });
}

void FTPSession::readFileDataAndSend(net::ip::tcp::socket& dataSocket,
                                     std::shared_ptr<IoFile> file) {
  net::post(fileRWStrand_, [me = shared_from_this(), &dataSocket, file]() {
    if (file->fileStream_.eof()) return;

    std::shared_ptr<std::vector<char>> buffer =
        std::make_shared<std::vector<char>>(1 << 20);
    file->fileStream_.read(buffer->data(),
                           static_cast<std::streamsize>(buffer->size()));
    auto bytes_read = file->fileStream_.gcount();
    buffer->resize(static_cast<size_t>(bytes_read));

    if (!file->fileStream_.eof()) {
      me->addDataToBufferAndSend(dataSocket, buffer, [me, &dataSocket, file]() {
        me->readFileDataAndSend(dataSocket, file);
      });
    } else {
      me->addDataToBufferAndSend(dataSocket, buffer);
      me->addDataToBufferAndSend(dataSocket,
                                 std::shared_ptr<std::vector<char>>(nullptr));
    }
  });
}

void FTPSession::addDataToBufferAndSend(net::ip::tcp::socket& dataSocket,
                                        std::shared_ptr<std::vector<char>> data,
                                        std::function<void(void)> fetchMore) {
  net::post(dataBufStrand_,
            [me = shared_from_this(), &dataSocket, data, fetchMore]() {
              bool writeInProgress = !me->dataBuffer_.empty();
              me->dataBuffer_.push_back(data);
              if (!writeInProgress) {
                me->writeDataToSocket(dataSocket, fetchMore);
              }
            });
}

void FTPSession::writeDataToSocket(net::ip::tcp::socket& dataSocket,
                                   std::function<void(void)> fetchMore) {
  net::post(dataBufStrand_, [me = shared_from_this(), &dataSocket,
                             fetchMore]() {
    auto data = me->dataBuffer_.front();
    if (data) {
      // Send out the buffer
      net::async_write(
          dataSocket, net::buffer(*data),
          net::bind_executor(
              me->dataBufStrand_, [me, &dataSocket, data, fetchMore](
                                      std::error_code const& ec,
                                      std::size_t /*bytes_to_transfer*/) {
                me->dataBuffer_.pop_front();
                if (ec) {
                  std::cerr << "Data write error: " << ec.message()
                            << std::endl;
                  return;
                }
                fetchMore();
                if (!me->dataBuffer_.empty()) {
                  me->writeDataToSocket(dataSocket, fetchMore);
                }
              }));
    } else {
      // we got to the end of transmission
      me->dataBuffer_.pop_front();
      me->sendFTPMsg(FTPMsgs(FTPReplyCode::CLOSING_DATA_CONNECTION, "Done"));
    }
  });
}

void FTPSession::receiveFile(std::shared_ptr<IoFile> file) {
  dataAcceptor_.async_accept(
      [file, me = shared_from_this()](std::error_code const& ec,
                                      net::ip::tcp::socket peer) {
        if (ec) {
          me->sendFTPMsg(
              FTPMsgs(FTPReplyCode::TRANSFER_ABORTED, "Data transfer aborted"));
          return;
        }
        me->receiveDataFromSocketAndWriteToFile(peer, file);
      });
}

void FTPSession::receiveDataFromSocketAndWriteToFile(
    net::ip::tcp::socket& dataSocket, std::shared_ptr<IoFile> file) {
  std::shared_ptr<std::vector<char>> buffer =
      std::make_shared<std::vector<char>>(1 << 20);

  net::async_read(
      dataSocket, net::buffer(*buffer), net::transfer_at_least(buffer->size()),
      [me = shared_from_this(), &dataSocket, file, buffer](
          std::error_code const& ec, std::size_t length) {
        buffer->resize(length);
        if (ec) {
          me->sendFTPMsg(
              FTPMsgs(FTPReplyCode::CLOSING_DATA_CONNECTION, "Done"));
          if (length > 0) {
            me->writeDataToFile(buffer, file);
          }
          return;
        } else if (length > 0) {
          me->writeDataToFile(buffer, file, [me, &dataSocket, file]() {
            me->receiveDataFromSocketAndWriteToFile(dataSocket, file);
          });
        }
      });
}

void FTPSession::writeDataToFile(std::shared_ptr<std::vector<char>> data,
                                 std::shared_ptr<IoFile> file,
                                 std::function<void(void)> fetchMore) {
  net::post(fileRWStrand_, [me = shared_from_this(), data, file, fetchMore] {
    fetchMore();
    file->fileStream_.write(data->data(),
                            static_cast<std::streamsize>(data->size()));
  });
}

// FTP data-socket send

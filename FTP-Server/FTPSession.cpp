#include <chrono>
#include <iostream>
#include <sstream>

#include "FTPSession.hpp"

static std::set<fs::path> dirContent(fs::path const& path) {
  assert(fs::is_directory(path));
  std::set<fs::path> content;
  for (auto const& entry : fs::directory_iterator(path)) {
    content.insert(entry.path());
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

static std::string timeString(fs::path const& path) {
  using namespace std::chrono;

  fs::file_time_type ftime = fs::last_write_time(path);
  auto sctp = time_point_cast<system_clock::duration>(
      ftime - decltype(ftime)::clock::now() + system_clock::now());
  std::time_t file_time_t = system_clock::to_time_t(sctp);

  auto now = system_clock::now();
  std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);

  std::tm now_timeinfo, file_timeinfo;
#if defined(__unix__)
  localtime_r(&now_time_t, &now_timeinfo);
  localtime_r(&file_time_t, &file_timeinfo);
#elif defined(_MSC_VER)
  localtime_s(&now_timeinfo, &now_time_t);
  localtime_s(&file_timeinfo, &file_time_t);
#else
  static std::mutex mtx;
  std::unique_lock<std::mutex> lock(mtx);
  now_timeinfo = *std::localtime(&now_time_t);
  file_timeinfo = *std::localtime(&file_time_t);
  lock.unlock();
#endif

  int current_year = now_timeinfo.tm_year, file_year = file_timeinfo.tm_year;

  std::stringstream ss;
  ss << (file_year == current_year ? std::put_time(&file_timeinfo, "%b %e %R")
                                   : std::put_time(&now_timeinfo, "%b %e  %Y"));
  // TODO 1 check time
  return ss.str();
}

FTPSession::FTPSession(
    net::io_context& context, net::ip::tcp::socket& cmdSocket,
    UserDatabase& userDb,
    std::function<void(session_ptr, bool)> const& contactHandler)
    : userDb_(userDb),
      context_(context),
      cmdSocket_(std::move(cmdSocket)),
      notiSocket_(context_),
      thisClientUploading_(false),
      dataTypeBinary_(true),
      msgWriteStrand_(context_.get_executor()),
      fileRWStrand_(context_.get_executor()),
      dataBufStrand_(context_.get_executor()),
      dataAcceptor_(context_),
      contactHandler_(contactHandler) {}

FTPSession::~FTPSession() {
  std::cout << "FTP Session shutting down" << std::endl;
  // TODO1 ua co ham stop() khong vay
  sessionUser_ = nullptr;
  if (thisClientUploading_) {
    isUploading_ = false;
  }
  contactHandler_(shared_from_this(), false);
}

std::atomic<bool> FTPSession::isUploading_(false);

std::string FTPSession::getUserName() const { return username_; }

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

void FTPSession::deliver(std::string const& msg) {
  if (notiSocket_.is_open()) {
    net::async_write(
        notiSocket_, net::buffer(msg),
        [](std::error_code const& ec, std::size_t /*bytes_to_transfer*/) {
          if (ec) {
            std::cerr << "Notification error: " << ec.message() << std::endl;
          }
        });
  }
}

void FTPSession::sendFTPMsg(FTPMsgs const& msg) {
  net::post(msgWriteStrand_, [me = shared_from_this(), msg]() {
    bool writeInProgress = !me->msgOutputQueue_.empty();
    me->msgOutputQueue_.push_back(msg.str());
    if (!writeInProgress) {
      me->startSendingMsgs();
    }
  });
}

void FTPSession::readFTPCmd() {
  cmdInputStr_.clear();
  net::async_read_until(
      cmdSocket_, net::dynamic_buffer(cmdInputStr_), "\r\n",
      net::bind_executor(
          msgWriteStrand_,
          [me = shared_from_this()](std::error_code const& ec, size_t length) {
            if (!ec) {
              std::string packetStr(me->cmdInputStr_, 0,
                                    length - 2);  // Remove \r\n
              me->cmdInputStr_.clear();
              std::cout << "FTP << " << packetStr << std::endl;
              me->handleFTPCmd(packetStr);
              return;
            }
            if (ec != net::error::eof) {
              std::cerr << ec.message() << std::endl;
            } else {
              std::cout << "Control connection closed by client" << std::endl;
            }
          }));
}

void FTPSession::startSendingMsgs() {
  std::cout << "FTP >> " << msgOutputQueue_.front() << std::endl;
  net::async_write(
      cmdSocket_, net::buffer(msgOutputQueue_.front()),
      net::bind_executor(
          msgWriteStrand_,
          [me = shared_from_this()](std::error_code const& ec,
                                    std::size_t /*bytes_to_transfer*/) {
            if (!ec) {
              me->msgOutputQueue_.pop_front();
              if (!me->msgOutputQueue_.empty()) {
                me->startSendingMsgs();
              }
            } else {
              std::cerr << "Message write error: " << ec.message() << std::endl;
            }
          }));
}

void FTPSession::handleFTPCmd(std::string const& cmd) {
  const std::map<std::string, std::function<FTPMsgs(std::string)>> cmdMap{
      // TODO1 make it static?
      {"UADD",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdUADD(para);
       }},
      {"USER",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdUSER(para);
       }},
      {"NOTI",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdNOTI(para);
       }},
      {"PASS",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdPASS(para);
       }},
      {"ACCT",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdACCT(para);
       }},
      {"CWD",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdCWD(para);
       }},
      {"CDUP",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdCDUP(para);
       }},
      {"REIN",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdREIN(para);
       }},
      {"QUIT",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdQUIT(para);
       }},
      // Transfer parameter commands
      {"PORT",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdPORT(para);
       }},
      {"PASV",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdPASV(para);
       }},
      {"TYPE",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdTYPE(para);
       }},
      {"STRU",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdSTRU(para);
       }},
      {"MODE",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdMODE(para);
       }},
      // Ftp service commands
      {"RETR",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdRETR(para);
       }},
      {"STOR",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdSTOR(para);
       }},
      {"SIZE",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdSIZE(para);
       }},
      {"STOU",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdSTOU(para);
       }},
      {"APPE",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdAPPE(para);
       }},
      {"ALLO",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdALLO(para);
       }},
      {"REST",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdREST(para);
       }},
      {"RNFR",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdRNFR(para);
       }},
      {"RNTO",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdRNTO(para);
       }},
      {"ABOR",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdABOR(para);
       }},
      {"DELE",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdDELE(para);
       }},
      {"RMD",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdRMD(para);
       }},
      {"MKD",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdMKD(para);
       }},
      {"PWD",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdPWD(para);
       }},
      {"LIST",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdLIST(para);
       }},
      {"NLST",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdNLST(para);
       }},
      {"SITE",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdSITE(para);
       }},
      {"SYST",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdSYST(para);
       }},
      {"STAT",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdSTAT(para);
       }},
      {"HELP",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdHELP(para);
       }},
      {"NOOP",
       [&](std::string const& para) -> FTPMsgs {
         return handleFTPCmdNOOP(para);
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
    contactHandler_(shared_from_this(), true);
    lastCmd_ = ftpCmd;
  } else {
    sendFTPMsg(FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                       "Unrecognized command"));
  }
  if (lastCmd_ == "QUIT") {
    // TODO1 check atomic
    net::bind_executor(msgWriteStrand_,
                       [me = shared_from_this()]() { me->cmdSocket_.close(); });
  } else {
    // Wait for next command
    readFTPCmd();
  }
}

fs::path FTPSession::FTP2LocalPath(fs::path const& ftpPath) const {
  assert(sessionUser_);
  fs::path path = ftpPath.has_root_directory()
                      ? sessionUser_->localRootPath_ / ftpPath.relative_path()
                      : ftpWorkingDir_ / ftpPath;
  path = fs::weakly_canonical(path);
  return path < sessionUser_->localRootPath_ ? sessionUser_->localRootPath_
                                             : path;
}

std::string FTPSession::Local2FTPPath(fs::path const& ftp_Path) const {
  assert(sessionUser_);
  if (ftp_Path == sessionUser_->localRootPath_) return "/";
  std::string ftp_path = ftp_Path.generic_string(),
              root_path = sessionUser_->localRootPath_.generic_string();
  return ftp_path.substr(root_path.find(ftp_path) + root_path.length() + 1);
}

FTPMsgs FTPSession::checkPathRenamable(fs::path const& ftpPath) const {
  // TODO1 check it
  if (!sessionUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (ftpPath.empty()) {
    return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_PARAMETERS, "Empty path");
  }
  fs::path localPath = FTP2LocalPath(ftpPath);
  try {
    if (fs::exists(localPath)) {
      if (fs::is_directory(localPath)) {
        auto fs = fs::directory_iterator(localPath);
      }
      // No read permission -> throw
      return FTPMsgs(FTPReplyCode::COMMAND_OK, "");
    } else {
      return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "File does not exist");
    }
  } catch (fs::filesystem_error const&) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "Permission denied");
  }
}

void FTPSession::sendDirListing(std::set<fs::path> const& dirContent) {
  dataAcceptor_.async_accept(
      [me = shared_from_this(), dirContent](std::error_code const& ec,
                                            net::ip::tcp::socket peer) {
        if (ec) {
          me->sendFTPMsg(
              FTPMsgs(FTPReplyCode::TRANSFER_ABORTED, "Data transfer aborted"));
          return;
        }
        socket_ptr socketPtr(
            std::make_shared<net::ip::tcp::socket>(std::move(peer)));

        std::stringstream stream;
        std::string ownerStr = "hcmus", groupStr = "hcmus";
        for (auto const& entry : dirContent) {
          // hcmus hcmus <size> <timestring> <filename>
          stream << (fs::is_directory(entry) ? 'd' : '-')
                 << permString(fs::status(entry).permissions()) << "   1 ";

          stream << std::setw(10) << ownerStr << " " << std::setw(10)
                 << groupStr << " ";
          stream << std::setw(10) << fs::file_size(entry) << " ";
          stream << timeString(entry) << " ";
          stream << entry.filename().string() << "\r\n";
        }
        // Copy the file list into a raw char vector
        std::string dirListStr = stream.str();
        charbuf_ptr rawDirListData(
            std::make_shared<std::vector<char>>(dirListStr.size()));
        std::copy(dirListStr.begin(), dirListStr.end(),
                  std::back_inserter(*rawDirListData));
        // Send the string out
        me->addDataToBufferAndSend(socketPtr, rawDirListData);
        me->addDataToBufferAndSend(
            socketPtr, nullptr);  // Nullpointer indicates end of transmission
      });
}

void FTPSession::sendNameList(std::set<fs::path> const& dirContent) {
  dataAcceptor_.async_accept([me = shared_from_this(), dirContent](
                                 std::error_code const& ec,
                                 net::ip::tcp::socket peer) {
    if (ec) {
      me->sendFTPMsg(
          FTPMsgs(FTPReplyCode::TRANSFER_ABORTED, "Data transfer aborted"));
      return;
    }
    socket_ptr dataSocketPtr(
        std::make_shared<net::ip::tcp::socket>(std::move(peer)));
    // Create a file list
    std::stringstream stream;
    for (const auto& entry : dirContent) {
      stream << entry.filename() << "\r\n";
    }

    // Copy the file list into a raw char vector
    std::string dirListStr = stream.str();
    charbuf_ptr rawDirListData(
        std::make_shared<std::vector<char>>(dirListStr.size()));
    std::copy(dirListStr.begin(), dirListStr.end(),
              std::back_inserter(*rawDirListData));

    // Send the string out
    me->addDataToBufferAndSend(dataSocketPtr, rawDirListData);
    me->addDataToBufferAndSend(
        dataSocketPtr, nullptr);  // Nullpointer indicates end of transmission
  });
}

// FTP Commands
// Access control commands
FTPMsgs FTPSession::handleFTPCmdUADD(std::string const& param) {
  sessionUser_ = nullptr;
  username_ = param;
  return param.empty()
             ? FTPMsgs(FTPReplyCode::SYNTAX_ERROR_PARAMETERS,
                       "Please provide username")
             : FTPMsgs(FTPReplyCode::USER_NAME_OK, "Please enter new password");
}

FTPMsgs FTPSession::handleFTPCmdUSER(std::string const& param) {
  sessionUser_ = nullptr;
  username_ = param;
  return param.empty()
             ? FTPMsgs(FTPReplyCode::SYNTAX_ERROR_PARAMETERS,
                       "Please provide username")
             : FTPMsgs(FTPReplyCode::USER_NAME_OK, "Please enter password");
}

FTPMsgs FTPSession::handleFTPCmdNOTI(std::string const& para) {
  if (notiSocket_.is_open()) {
    notiSocket_.shutdown(net::ip::tcp::socket::shutdown_both);
    notiSocket_.close();
  }
  uint16_t port = std::stoi(para);
  net::ip::tcp::endpoint notiEndpoint(cmdSocket_.local_endpoint().address(),
                                      port);
  notiSocket_.async_connect(notiEndpoint, [](std::error_code const& er) {
    if (er) {
      std::cerr << "Connect to notification socket failed: " << er.message()
                << std::endl;
    } else {
      std::cout << "Connected to notification socket" << std::endl;
    }
  });
  return FTPMsgs(FTPReplyCode::COMMAND_OK, "");
}

FTPMsgs FTPSession::handleFTPCmdPASS(std::string const& param) {
  if (lastCmd_ == "USER") {
    if (auto user = userDb_.getUser(username_, param); user) {
      sessionUser_ = user;
      ftpWorkingDir_ = user->localRootPath_;
      // TODO1 thong bao login chac la cho nay
      return FTPMsgs(FTPReplyCode::USER_LOGGED_IN, "Login successfully");
    } else {
      return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Failed to log in");
    }
  } else if (lastCmd_ == "UADD") {
    // TODO1 choose root dir
    if (auto user = userDb_.addUser(username_, param, fs::current_path());
        user) {
      sessionUser_ = user;
      ftpWorkingDir_ = user->localRootPath_;
      contactHandler_(shared_from_this(), true);
      return FTPMsgs(FTPReplyCode::USER_LOGGED_IN, "Sign up successfully");
    } else {
      return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Username already existed");
    }
  } else {
    return FTPMsgs(FTPReplyCode::COMMANDS_BAD_SEQUENCE,
                   "Please specify username first");
  }
}

FTPMsgs FTPSession::handleFTPCmdACCT(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                 "Unsupported command");
}

FTPMsgs FTPSession::handleFTPCmdCWD(std::string const& param) {
  if (!sessionUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (param.empty()) {
    return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_PARAMETERS, "No path given");
  }

  fs::path absNewWorkingDir = FTP2LocalPath(param);
  // TODO3 network drive
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
        "Failed changing directory: The given resource is not a directory.");
  }

  try {
    auto dirIt = fs::directory_iterator(absNewWorkingDir);
  } catch (fs::filesystem_error const&) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN,
                   "Failed changing directory: Permission denied.");
  }

  ftpWorkingDir_ = absNewWorkingDir;
  return FTPMsgs(
      FTPReplyCode::FILE_ACTION_COMPLETED,
      "Working directory changed to " + fs::path(param).generic_string());
}

FTPMsgs FTPSession::handleFTPCmdCDUP(std::string const& /*param*/) {
  if (!sessionUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (ftpWorkingDir_ != sessionUser_->localRootPath_) {
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
  // TODO1 neu loggerUser khac null thi thong bao
  sessionUser_ = nullptr;
  contactHandler_(shared_from_this(), false);
  if (thisClientUploading_) isUploading_ = false;
  return FTPMsgs(FTPReplyCode::SERVICE_CLOSING_CONTROL_CONNECTION,
                 "Connection shutting down");
}

// Transfer parameter commands
FTPMsgs FTPSession::handleFTPCmdPORT(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                 "FTP active mode is not supported by this server");
}

FTPMsgs FTPSession::handleFTPCmdPASV(std::string const& /*param*/) {
  if (!sessionUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  try {
    // TODO1 open roi thi khong can mo lai
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
  std::stringstream stream;
  stream << '(';
  for (int i = 0; i < 4; ++i) {
    stream << static_cast<int>(ipBytes[i]) << ",";
  }
  stream << ((port >> 8) & 0xff) << "," << (port & 0xff) << ")";
  return FTPMsgs(FTPReplyCode::ENTERING_PASSIVE_MODE,
                 "Entering passive mode " + stream.str());
}

FTPMsgs FTPSession::handleFTPCmdTYPE(std::string const& param) {
  if (!sessionUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (param == "A") {
    dataTypeBinary_ = false;
    // TODO3: The ASCII mode currently does not work as RFC 959 demands it. It
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
  if (!sessionUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }

  if (!dataAcceptor_.is_open()) {
    return FTPMsgs(FTPReplyCode::ERROR_OPENING_DATA_CONNECTION,
                   "Error opening data connection");
  }

  fs::path localPath = FTP2LocalPath(param);
  std::ios::openmode openMode =
      (dataTypeBinary_ ? (std::ios::in | std::ios::binary) : (std::ios::in));
  ioFile_ptr file(std::make_shared<IoFile>(localPath, openMode));
  if (!file->fileStream_.good()) {
    return FTPMsgs(FTPReplyCode::ACTION_ABORTED_LOCAL_ERROR,
                   "Error opening file for transfer");
  }
  sendFile(file);
  return FTPMsgs(FTPReplyCode::FILE_STATUS_OK_OPENING_DATA_CONNECTION,
                 "Sending file");
}

FTPMsgs FTPSession::handleFTPCmdSTOR(std::string const& param) {
  if (!sessionUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  // TODO3: the ACTION_NOT_TAKEN reply is not RCF 959 conform. Apparently in
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
  } catch (fs::filesystem_error const&) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN_FILENAME_NOT_ALLOWED,
                   "Cannot read file status.");
  }

  std::ios::openmode openMode =
      (dataTypeBinary_ ? (std::ios::out | std::ios::binary) : (std::ios::out));
  ioFile_ptr file(std::make_shared<IoFile>(localPath, openMode));
  if (!file->fileStream_.good()) {
    return FTPMsgs(FTPReplyCode::ACTION_ABORTED_LOCAL_ERROR,
                   "Error opening file for transfer");
  }
  if (isUploading_) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN_FILENAME_NOT_ALLOWED,
                   "Another client is uploading.");
  }
  isUploading_ = true;
  thisClientUploading_ = true;
  receiveFile(file);
  thisClientUploading_ = false;
  isUploading_ = false;

  return FTPMsgs(FTPReplyCode::FILE_STATUS_OK_OPENING_DATA_CONNECTION,
                 "Receiving file");
}

FTPMsgs FTPSession::handleFTPCmdSIZE(std::string const& para) {
  if (!sessionUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }
  if (!dataAcceptor_.is_open()) {
    return FTPMsgs(FTPReplyCode::ERROR_OPENING_DATA_CONNECTION,
                   "Error opening data connection");
  }

  fs::path localPath = FTP2LocalPath(para);
  return fs::exists(localPath)
             ? FTPMsgs(FTPReplyCode::FILE_STATUS,
                       std::to_string(fs::file_size(localPath)))
             : FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN,
                       "Failed read file's size");
}

FTPMsgs FTPSession::handleFTPCmdSTOU(std::string const& /*param*/) {
  return FTPMsgs(FTPReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND,
                 "Command not implemented");
}

FTPMsgs FTPSession::handleFTPCmdAPPE(std::string const& param) {
  if (!sessionUser_) {
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
  } catch (fs::filesystem_error const&) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "Cannot read file status.");
  }

  std::ios::openmode openMode =
      (dataTypeBinary_ ? (std::ios::out | std::ios::app | std::ios::binary)
                       : (std::ios::out | std::ios::app));
  ioFile_ptr file(std::make_shared<IoFile>(localPath, openMode));

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
  if (!sessionUser_) {
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

  // TODO3: returning neiher FILE_ACTION_NOT_TAKEN nor ACTION_NOT_TAKEN are
  // RFC 959 conform. Aoarently back in 1985 it was assumed that the RNTO
  // command will always succeed, as long as you enter a valid target file
  // name. Thus we use the two return codes anyways, the popular FileZilla
  // FTP Server uses those as well.
  if (FTPMsgs isRenamableErr = checkPathRenamable(renameSrcPath_);
      isRenamableErr.replyCode() == FTPReplyCode::COMMAND_OK) {
    fs::path localSrcPath = FTP2LocalPath(renameSrcPath_),
             localDstPath = FTP2LocalPath(param);
    // Check if the source file exists already. We simple disallow overwriting
    // a file be renaming (the bahavior of the native rename command on
    // Windows and Linux differs; Windows will not overwrite files, Linux
    // will).
    if (fs::exists(localDstPath)) {
      return FTPMsgs(FTPReplyCode::FILE_ACTION_NOT_TAKEN,
                     "Target path exists already.");
    }

    std::error_code ec;
    // TODO2 catch????
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
  if (!sessionUser_) {
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
  if (!sessionUser_) {
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
  if (!sessionUser_) {
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
  if (!sessionUser_) {
    return FTPMsgs(FTPReplyCode::ACTION_NOT_TAKEN, "Not logged in");
  }
  // TODO1 bo root trong cai duoi
  return FTPMsgs(FTPReplyCode::PATHNAME_CREATED, Local2FTPPath(ftpWorkingDir_));
}

FTPMsgs FTPSession::handleFTPCmdLIST(std::string const& param) {
  if (!sessionUser_) {
    return FTPMsgs(FTPReplyCode::NOT_LOGGED_IN, "Not logged in");
  }

  fs::path localPath = FTP2LocalPath(param);
  try {
    if (fs::exists(localPath)) {
      if (fs::is_directory(localPath)) {
        sendDirListing(dirContent(localPath));
        return FTPMsgs(FTPReplyCode::FILE_STATUS_OK_OPENING_DATA_CONNECTION,
                       "Sending directory list");
      } else {
        // TODO3: RFC959: If the pathname specifies a file then the server
        // should send current information on the file.
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
  if (!sessionUser_) {
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
        // TODO3: RFC959: If the pathname specifies a file then the server
        // should send current information on the file.
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

void FTPSession::sendFile(ioFile_ptr const& file) {
  dataAcceptor_.async_accept(
      [me = shared_from_this(), file](std::error_code const& ec,
                                      net::ip::tcp::socket peer) {
        if (ec) {
          me->sendFTPMsg(
              FTPMsgs(FTPReplyCode::TRANSFER_ABORTED, "Data transfer aborted"));
          return;
        }
        socket_ptr dataSocketPtr(
            std::make_shared<net::ip::tcp::socket>(std::move(peer)));
        // Start sending multiple buffers at once
        me->readFileDataAndSend(dataSocketPtr, file);
        me->readFileDataAndSend(dataSocketPtr, file);
        me->readFileDataAndSend(dataSocketPtr, file);
      });
}

void FTPSession::readFileDataAndSend(socket_ptr const& dataSocketPtr,
                                     ioFile_ptr const& file) {
  net::post(fileRWStrand_, [me = shared_from_this(), dataSocketPtr, file]() {
    if (file->fileStream_.eof()) {
      return;
    }
    charbuf_ptr buffer(std::make_shared<std::vector<char>>(1 << 20));
    file->fileStream_.read(buffer->data(), buffer->size());
    buffer->resize(file->fileStream_.gcount());

    if (!file->fileStream_.eof()) {
      me->addDataToBufferAndSend(dataSocketPtr, buffer,
                                 [me, dataSocketPtr, file]() {
                                   me->readFileDataAndSend(dataSocketPtr, file);
                                 });
    } else {
      me->addDataToBufferAndSend(dataSocketPtr, buffer);
      me->addDataToBufferAndSend(dataSocketPtr, nullptr);
    }
  });
}

void FTPSession::addDataToBufferAndSend(socket_ptr const& dataSocketPtr,
                                        charbuf_ptr const& data,
                                        std::function<void(void)> fetchMore) {
  net::post(dataBufStrand_,
            [me = shared_from_this(), dataSocketPtr, data, fetchMore]() {
              bool writeInProgress = !me->dataBuffer_.empty();
              me->dataBuffer_.push_back(data);
              if (!writeInProgress) {
                me->writeDataToSocket(dataSocketPtr, fetchMore);
              }
            });
}

void FTPSession::writeDataToSocket(socket_ptr const& dataSocketPtr,
                                   std::function<void(void)> fetchMore) {
  net::post(dataBufStrand_, [me = shared_from_this(), dataSocketPtr,
                             fetchMore]() {
    if (auto data = me->dataBuffer_.front(); data) {
      net::async_write(
          *dataSocketPtr, net::buffer(*data),
          net::bind_executor(
              me->dataBufStrand_, [me, dataSocketPtr, data, fetchMore](
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
                  me->writeDataToSocket(dataSocketPtr, fetchMore);
                }
              }));
    } else {
      // we got to the end of transmission
      me->dataBuffer_.pop_front();
      me->sendFTPMsg(FTPMsgs(FTPReplyCode::CLOSING_DATA_CONNECTION, "Done"));
    }
  });
}

void FTPSession::receiveFile(ioFile_ptr const& file) {
  dataAcceptor_.async_accept(
      [me = shared_from_this(), file](std::error_code const& ec,
                                      net::ip::tcp::socket peer) {
        if (ec) {
          me->sendFTPMsg(
              FTPMsgs(FTPReplyCode::TRANSFER_ABORTED, "Data transfer aborted"));
          return;
        }
        socket_ptr dataSocketPtr(
            std::make_shared<net::ip::tcp::socket>(std::move(peer)));
        me->receiveDataFromSocketAndWriteToFile(dataSocketPtr, file);
      });
}

void FTPSession::receiveDataFromSocketAndWriteToFile(
    socket_ptr const& dataSocketPtr, ioFile_ptr const& file) {
  charbuf_ptr buffer = std::make_shared<std::vector<char>>(1 << 20);
  net::async_read(
      *dataSocketPtr, net::buffer(*buffer),
      net::transfer_at_least(buffer->size()),
      [me = shared_from_this(), dataSocketPtr, buffer, file](
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
          me->writeDataToFile(buffer, file, [me, dataSocketPtr, file]() {
            me->receiveDataFromSocketAndWriteToFile(dataSocketPtr, file);
          });
        }
      });
}

void FTPSession::writeDataToFile(charbuf_ptr const& data,
                                 ioFile_ptr const& file,
                                 std::function<void(void)> fetchMore) {
  net::post(fileRWStrand_, [me = shared_from_this(), data, file, fetchMore] {
    fetchMore();
    file->fileStream_.write(data->data(), data->size());
  });
}

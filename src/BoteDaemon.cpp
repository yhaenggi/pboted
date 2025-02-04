/**
 * Copyright (c) 2019-2021 polistern
 */

#include "BoteContext.h"
#include "ConfigParser.h"
#include "BoteDaemon.h"

#include <memory>
#include "DHTworker.h"
#include "EmailWorker.h"
#include "FileSystem.h"
#include "Logging.h"
#include "POP3.h"
#include "RelayPeersWorker.h"
#include "SMTP.h"
#include "version.h"

namespace pbote {
namespace util {

class Daemon_Singleton::Daemon_Singleton_Private {
 public:
  Daemon_Singleton_Private() {};
  ~Daemon_Singleton_Private() {};

  std::unique_ptr<bote::smtp::SMTP> SMTPserver;
  std::unique_ptr<bote::pop3::POP3> POP3server;
};

Daemon_Singleton::Daemon_Singleton()
    : isDaemon(false), running(true), d(*new Daemon_Singleton_Private()) {}
Daemon_Singleton::~Daemon_Singleton() { delete &d; }

bool Daemon_Singleton::IsService() const {
  bool service = false;
  pbote::config::GetOption("service", service);
  return service;
}

bool Daemon_Singleton::init(int argc, char *argv[]) {
  return init(argc, argv, nullptr);
}

bool Daemon_Singleton::init(int argc, char *argv[],
                            std::shared_ptr<std::ostream> logstream) {
  pbote::config::Init();
  pbote::config::ParseCmdline(argc, argv);

  std::string config;
  pbote::config::GetOption("conf", config);
  std::string datadir;
  pbote::config::GetOption("datadir", datadir);
  pbote::fs::DetectDataDir(datadir, IsService());
  pbote::fs::Init();

  datadir = pbote::fs::GetDataDir();
  if (config.empty()) {
    config = pbote::fs::DataDirPath("pboted.conf");
    if (!pbote::fs::Exists(config))
      config = "";
  }

  pbote::config::ParseConfig(config);
  pbote::config::Finalize();

  pbote::config::GetOption("daemon", isDaemon);

  std::string logs;
  pbote::config::GetOption("log", logs);
  std::string logfile;
  pbote::config::GetOption("logfile", logfile);
  std::string loglevel;
  pbote::config::GetOption("loglevel", loglevel);
  bool logclftime;
  pbote::config::GetOption("logclftime", logclftime);

  /* setup logging */
  if (logclftime)
    pbote::log::Logger().SetTimeFormat("[%d/%b/%Y:%H:%M:%S %z]");

  if (isDaemon && (logs.empty() || logs == "stdout"))
    logs = "file";

  pbote::log::Logger().SetLogLevel(loglevel);
  if (logstream) {
    LogPrint(eLogInfo, "Log: will send messages to std::ostream");
    pbote::log::Logger().SendTo(logstream);
  } else if (logs == "file") {
    if (logfile.empty())
      logfile = pbote::fs::DataDirPath("pboted.log");
    LogPrint(eLogInfo, "Log: will send messages to ", logfile);
    pbote::log::Logger().SendTo(logfile);
  } else if (logs == "syslog") {
    LogPrint(eLogInfo, "Log: will send messages to syslog");
    pbote::log::Logger().SendTo("pboted", LOG_DAEMON);
  } else {
    // use stdout -- default
  }

  LogPrint(eLogInfo, "pBoteD v", VERSION, " starting");
  LogPrint(eLogDebug, "FS: data directory: ", datadir);
  LogPrint(eLogDebug, "FS: main config file: ", config);

  LogPrint(eLogInfo, "Daemon: init context");
  pbote::context.init();

  LogPrint(eLogInfo, "Daemon: init network");
  pbote::network::network_worker.init();

  LogPrint(eLogDebug, "Daemon: init done");
  return true;
}

int Daemon_Singleton::start() {
  LogPrint(eLogDebug, "Daemon: start services");
  pbote::log::Logger().Start();

  LogPrint(eLogInfo, "Daemon: starting network worker");
  pbote::network::network_worker.start();

  LogPrint(eLogInfo, "Daemon: starting packet handler");
  pbote::packet::packet_handler.start();

  LogPrint(eLogInfo, "Daemon: starting relay peers");
  pbote::relay::relay_peers_worker.start();

  LogPrint(eLogInfo, "Daemon: starting DHT");
  pbote::kademlia::DHT_worker.start();

  LogPrint(eLogInfo, "Daemon: starting Email");
  pbote::kademlia::email_worker.start();

  bool smtp;
  pbote::config::GetOption("smtp.enabled", smtp);
  if (smtp) {
    std::string SMTPaddr;
    uint16_t SMTPport;
    pbote::config::GetOption("smtp.address", SMTPaddr);
    pbote::config::GetOption("smtp.port", SMTPport);
    LogPrint(eLogInfo, "Daemon: starting SMTP server at ",
             SMTPaddr, ":", SMTPport);

    try {
      d.SMTPserver = std::make_unique<bote::smtp::SMTP>(SMTPaddr, SMTPport);
      d.SMTPserver->start();
    } catch (std::exception &ex) {
      LogPrint(eLogError, "Daemon: failed to start SMTP server: ",
               ex.what());
      ThrowFatal("Unable to start SMTP server at ",
                 SMTPaddr, ":", SMTPport, ": ", ex.what());
    }
  }

  bool pop3;
  pbote::config::GetOption("pop3.enabled", pop3);
  if (pop3) {
    std::string POP3addr;
    uint16_t POP3port;
    pbote::config::GetOption("pop3.address", POP3addr);
    pbote::config::GetOption("pop3.port", POP3port);
    LogPrint(eLogInfo, "Daemon: starting POP3 server at ",
             POP3addr, ":", POP3port);

    try {
      d.POP3server = std::make_unique<bote::pop3::POP3>(POP3addr, POP3port);
      d.POP3server->start();
    } catch (std::exception &ex) {
      LogPrint(eLogError, "Daemon: failed to start POP3 server: ",
               ex.what());
      ThrowFatal("Unable to start POP3 server at ",
                 POP3addr, ":", POP3port, ": ", ex.what());
    }
  }

  LogPrint(eLogInfo, "Daemon: started");

  return EXIT_SUCCESS;
}

bool Daemon_Singleton::stop() {
  LogPrint(eLogInfo, "Daemon: start shutting down");

  if (d.SMTPserver) {
    LogPrint(eLogInfo, "Daemon: stopping SMTP Server");
    d.SMTPserver->stop();
    d.SMTPserver = nullptr;
  }

  if (d.POP3server) {
    LogPrint(eLogInfo, "Daemon: stopping POP3 Server");
    d.POP3server->stop();
    d.POP3server = nullptr;
  }

  LogPrint(eLogInfo, "Daemon: stopping Email worker");
  pbote::kademlia::email_worker.stop();

  LogPrint(eLogInfo, "Daemon: stopping DHT worker");
  pbote::kademlia::DHT_worker.stop();

  LogPrint(eLogInfo, "Daemon: stopping relay peers worker");
  pbote::relay::relay_peers_worker.stop();

  // Looks like m_recvQueue->GetNext() hold stopping
  //LogPrint(eLogInfo, "Daemon: stopping packet handler");
  //pbote::packet::packet_handler.stop();

  LogPrint(eLogInfo, "Daemon: stopping network worker");
  pbote::network::network_worker.stop();

  pbote::log::Logger().Stop();

  return true;
}

} // namespace util
} // namespace pbote

/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <glib.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <signal.h>

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/flags/parse.h"
#include "api/scoped_refptr.h"
#include "examples/peerconnection/client/conductor.h"
#include "examples/peerconnection/client/flag_defs.h"
#include "examples/peerconnection/client/linux/main_wnd.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/field_trial.h"
#include "test/field_trial.h"

class CustomSocketServer : public rtc::PhysicalSocketServer {
 public:
  explicit CustomSocketServer(GtkMainWnd* wnd, bool disable_gui)
      : wnd_(wnd), conductor_(NULL), client_(NULL), disable_gui_(disable_gui), quit_(false) {}
  virtual ~CustomSocketServer() {}

  void SetMessageQueue(rtc::Thread* queue) override { message_queue_ = queue; }

  void set_client(PeerConnectionClient* client) { client_ = client; }
  void set_conductor(Conductor* conductor) { conductor_ = conductor; }

  // Override so that we can also pump the GTK message loop.
  // This function never waits.
  bool Wait(webrtc::TimeDelta max_wait_duration, bool process_io) override {
    if (!disable_gui_) {
      // Pump GTK events, keeps the thread alive.
      while (gtk_events_pending())
        gtk_main_iteration();
      if (!wnd_->IsWindow() && !conductor_->connection_active() &&
          client_ != NULL && !client_->is_connected()) {
        message_queue_->Quit();
      }
    } else {
      // Non-GUI mode handling
      if (message_queue_) {
        // event queue is stored in message_queue_
        // checks and processes any messages in the queue        
        // message_queue_->ProcessMessages(10);  // Process for 10ms
      } else {
        RTC_LOG(LS_WARNING) << "message_queue_ is null in non-GUI mode";
      }      
      if (quit_) {
        message_queue_->Quit();
      }
    }

    return rtc::PhysicalSocketServer::Wait(
        disable_gui_ ? max_wait_duration : webrtc::TimeDelta::Zero(), 
        process_io);
  }

  void Quit() { quit_ = true; }

  bool IsQuitting() const { return quit_; }

 protected:
  rtc::Thread* message_queue_;
  GtkMainWnd* wnd_;
  Conductor* conductor_;
  PeerConnectionClient* client_;
  bool disable_gui_;
  bool quit_;  
};

CustomSocketServer* g_socket_server = nullptr;

void SignalHandler(int signum) {
  if (g_socket_server) {
    printf("Received signal %d, initiating shutdown...\n", signum);
    g_socket_server->Quit();
  }
}

// Function to parse the configuration file
std::unordered_map<std::string, std::string> ParseConfigFile(const std::string& filename) {
  std::unordered_map<std::string, std::string> config;
  std::ifstream file(filename);
  
  if (!file.is_open()) {
    std::cerr << "Error: Unable to open config file: " << filename << std::endl;
    return config;
  }

  std::string line;
  while (std::getline(file, line)) {
    // Skip empty lines and comments
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::vector<std::string> parts = absl::StrSplit(line, absl::MaxSplits(':', 1));
    if (parts.size() == 2) {
      std::string key = std::string(absl::StripAsciiWhitespace(parts[0]));
      std::string value = std::string(absl::StripAsciiWhitespace(parts[1]));
      
      // Remove any inline comments from the value
      size_t comment_pos = value.find('#');
      if (comment_pos != std::string::npos) {
        value = std::string(absl::StripAsciiWhitespace(value.substr(0, comment_pos)));
      }

      config[key] = value;
    }
  }

  return config;
}


int main(int argc, char* argv[]) {
  gtk_init(&argc, &argv);
// g_type_init API is deprecated (and does nothing) since glib 2.35.0, see:
// https://mail.gnome.org/archives/commits-list/2012-November/msg07809.html
#if !GLIB_CHECK_VERSION(2, 35, 0)
  g_type_init();
#endif
// g_thread_init API is deprecated since glib 2.31.0, see release note:
// http://mail.gnome.org/archives/gnome-announce-list/2011-October/msg00041.html
#if !GLIB_CHECK_VERSION(2, 31, 0)
  g_thread_init(NULL);
#endif

  absl::ParseCommandLine(argc, argv);

  // Parse the configuration file
  std::string config_file = absl::GetFlag(FLAGS_config_file).empty() ? "./client.cfg" : absl::GetFlag(FLAGS_config_file);
  std::cout << "Attempting to read config file: " << config_file << std::endl;
  auto config = ParseConfigFile(config_file); 

  // InitFieldTrialsFromString stores the char*, so the char array must outlive
  // the application.
  const std::string forced_field_trials = absl::GetFlag(FLAGS_force_fieldtrials);
  webrtc::field_trial::InitFieldTrialsFromString(forced_field_trials.c_str());

  // Use configuration values, falling back to command-line flags if not present
  std::string server = config.count("server_ip") ? config["server_ip"] : absl::GetFlag(FLAGS_server);
  int port = config.count("server_port") ? std::stoi(config["server_port"]) : absl::GetFlag(FLAGS_port);
  bool autoconnect = config.count("autoconnect") ? (config["autoconnect"] == "true") : absl::GetFlag(FLAGS_autoconnect);
  bool autocall = config.count("autocall") ? (config["autocall"] == "true") : absl::GetFlag(FLAGS_autocall);
  bool disable_gui = config.count("disable_gui") ? (config["disable_gui"] == "true") : absl::GetFlag(FLAGS_disable_gui);
  bool is_caller = config.count("is_caller") ? (config["is_caller"] == "true") : absl::GetFlag(FLAGS_is_caller);
  std::string stun_server_ip = config.count("stun_server_ip") ? config["stun_server_ip"] : "stun.l.google.com";
  int stun_server_port = config.count("stun_server_port") ? std::stoi(config["stun_server_port"]) : 19302;

  // Print final values
  std::cout << "\nFinal configuration values:" << std::endl;
  std::cout << "server_ip: " << server << std::endl;
  std::cout << "server_port: " << port << std::endl;
  std::cout << "autoconnect: " << (autoconnect ? "true" : "false") << std::endl;
  std::cout << "autocall: " << (autocall ? "true" : "false") << std::endl;
  std::cout << "disable_gui: " << (disable_gui ? "true" : "false") << std::endl;
  std::cout << "is_caller: " << (is_caller ? "true" : "false") << std::endl;
  std::cout << "stun_server_ip: " << stun_server_ip << std::endl;
  std::cout << "stun_server_port: " << stun_server_port << std::endl;

  // Validate port
  if (port < 1 || port > 65535) {
    printf("Error: %i is not a valid port.\n", port);
    return -1;
  }

  RTC_LOG(LS_INFO) << "disable_gui flag: " << (disable_gui ? "true" : "false");

  GtkMainWnd wnd(server.c_str(), port, autoconnect, autocall, disable_gui);
  wnd.Create();

  CustomSocketServer socket_server(&wnd, disable_gui);
  g_socket_server = &socket_server;
  rtc::AutoSocketServerThread thread(&socket_server);

  // Set up signal handler for graceful termination
  signal(SIGINT, SignalHandler);

  rtc::InitializeSSL();
  // Must be constructed after we set the socketserver.
  PeerConnectionClient client;
  // create conductor
  auto conductor = rtc::make_ref_counted<Conductor>(&client, &wnd, disable_gui, is_caller);
  conductor->SetStunServer(stun_server_ip, stun_server_port);
  socket_server.set_client(&client);
  socket_server.set_conductor(conductor.get());

  // Automatically start login process in GUI-less mode
  if (disable_gui) {
    conductor->AutoLogin(server, absl::GetFlag(FLAGS_port));

    // Run the thread until it's time to quit
    while (!socket_server.IsQuitting()) {
      conductor->ProcessMessagesForNonGUIMode();

      // Existing message processing
      thread.ProcessMessages(10);
    }
  } else {
    // This method starts the thread's message loop and blocks until the thread is quit.
    // event driven
    thread.Run();
  }

  printf("Exiting main loop\n");

  wnd.Destroy();
  rtc::CleanupSSL();
  g_socket_server = nullptr;
  return 0;
}
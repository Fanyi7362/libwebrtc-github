/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/client/peer_connection_client.h"

#include "api/units/time_delta.h"
#include "examples/peerconnection/client/defaults.h"
#include "rtc_base/async_dns_resolver.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/net_helpers.h"
#include "rtc_base/thread.h"

namespace {

// This is our magical hangup signal.
constexpr char kByeMessage[] = "BYE";
// Delay between server connection retries, in milliseconds
constexpr webrtc::TimeDelta kReconnectDelay = webrtc::TimeDelta::Seconds(2);

rtc::Socket* CreateClientSocket(int family) {
  rtc::Thread* thread = rtc::Thread::Current();
  RTC_DCHECK(thread != NULL);
  return thread->socketserver()->CreateSocket(family, SOCK_STREAM);
}

}  // namespace

PeerConnectionClient::PeerConnectionClient()
    : callback_(NULL), resolver_(nullptr), state_(NOT_CONNECTED), my_id_(-1) {}

PeerConnectionClient::~PeerConnectionClient() = default;

// uses sigslot?
void PeerConnectionClient::InitSocketSignals() {
  RTC_DCHECK(control_socket_.get() != NULL);
  RTC_DCHECK(hanging_get_.get() != NULL);
  control_socket_->SignalCloseEvent.connect(this,
                                            &PeerConnectionClient::OnClose);
  hanging_get_->SignalCloseEvent.connect(this, &PeerConnectionClient::OnClose);
  control_socket_->SignalConnectEvent.connect(this,
                                              &PeerConnectionClient::OnConnect);
  hanging_get_->SignalConnectEvent.connect(
      this, &PeerConnectionClient::OnHangingGetConnect);
  control_socket_->SignalReadEvent.connect(this, &PeerConnectionClient::OnRead);
  hanging_get_->SignalReadEvent.connect(
      this, &PeerConnectionClient::OnHangingGetRead);
}

int PeerConnectionClient::id() const {
  return my_id_;
}

bool PeerConnectionClient::is_connected() const {
  return my_id_ != -1;
}

const Peers& PeerConnectionClient::peers() const {
  return peers_;
}

void PeerConnectionClient::RegisterObserver(
    PeerConnectionClientObserver* callback) {
  RTC_DCHECK(!callback_);
  callback_ = callback;   
}

void PeerConnectionClient::Connect(const std::string& server,
                                   int port,
                                   const std::string& client_name) {
  RTC_LOG(LS_INFO) << "Enter Connect";
  RTC_DCHECK(!server.empty());
  RTC_DCHECK(!client_name.empty());

  if (state_ != NOT_CONNECTED) {
    RTC_LOG(LS_WARNING)
        << "The client must not be connected before you can call Connect()";
    // call Conductor::OnServerConnectionFailure to inform Conductor the connection failed
    callback_->OnServerConnectionFailure();
    return;
  }

  if (server.empty() || client_name.empty()) {
    callback_->OnServerConnectionFailure();
    return;
  }

  if (port <= 0)
    port = kDefaultServerPort;

  // Store server address and port to be used later.
  server_address_.SetIP(server);
  server_address_.SetPort(port);
  client_name_ = client_name;

  // Resolve the address of the server IP.
  if (server_address_.IsUnresolvedIP()) {
    RTC_DCHECK_NE(state_, RESOLVING);
    RTC_DCHECK(!resolver_);
    state_ = RESOLVING;
    resolver_ = std::make_unique<webrtc::AsyncDnsResolver>(); // create a new AsyncDnsResolver
    resolver_->Start(server_address_,
                     [this] { 
                      RTC_LOG(LS_INFO) << "DNS resolution callback triggered";
                      OnResolveResult(resolver_->result()); 
                     });
  } else {
    DoConnect();
  }
  RTC_LOG(LS_INFO) << "Exit Connect";
}

// After the domain name resolver AsyncDnsResolver finishes, it will callback to this function.
void PeerConnectionClient::OnResolveResult(
    const webrtc::AsyncDnsResolverResult& result) {
  RTC_LOG(LS_INFO) << "Enter OnResolveResult";
  if (result.GetError() != 0) {
    callback_->OnServerConnectionFailure();
    resolver_.reset();
    state_ = NOT_CONNECTED;
    return;
  }
  if (!result.GetResolvedAddress(AF_INET, &server_address_)) {
    callback_->OnServerConnectionFailure();
    resolver_.reset();
    state_ = NOT_CONNECTED;
    return;
  }
  DoConnect();
  RTC_LOG(LS_INFO) << "Exit OnResolveResult";
}

void PeerConnectionClient::DoConnect() {
  // create async socket
  // control_socket_ is used to actively send signaling messages to the signaling server; 
  // hanging_get_ is used to request signaling messages from the signaling server, where 
  // it sends a wait signal to the signaling server and waits for a response. 
  // When the signaling server has messages to send to the client, it will return them 
  // through this socket.
  RTC_LOG(LS_INFO) << "Enter DoConnect";
  control_socket_.reset(CreateClientSocket(server_address_.ipaddr().family()));
  hanging_get_.reset(CreateClientSocket(server_address_.ipaddr().family()));
  InitSocketSignals();

  // PeerConnectionClient communicate with the server using HTTP.
  // prepare the HTTP GET sign_in request
  char buffer[1024];
  snprintf(buffer, sizeof(buffer), "GET /sign_in?%s HTTP/1.0\r\n\r\n",
           client_name_.c_str());
  onconnect_data_ = buffer;

  // initiate a connection to the server using the control socket
  bool ret = ConnectControlSocket();
  if (ret)
    state_ = SIGNING_IN; // update the state to SIGNING_IN
  if (!ret) {
    callback_->OnServerConnectionFailure();
  }
}

// PeerConnectionClient will package the signaling message into HTTP 
// and send it to the signaling server using the HTTP protocol.
// An example of offer signaling from client1 to server:
// POST /message?peer_id=1&to=2 HTTP/1.0
// Content-Length: 4197
// Content-Type: text/plain

// {
//    "sdp" : "v=0\r\no=- 7038993275920826226 ...",
//    "type" : "offer"
// }
// 
// the server will then forward this message to client2, 
// as a response to the wait signal.

bool PeerConnectionClient::SendToPeer(int peer_id, const std::string& message) {
  if (state_ != CONNECTED)
    return false;

  RTC_DCHECK(is_connected());
  RTC_DCHECK(control_socket_->GetState() == rtc::Socket::CS_CLOSED);
  if (!is_connected() || peer_id == -1)
    return false;

  char headers[1024];
  snprintf(headers, sizeof(headers),
           "POST /message?peer_id=%i&to=%i HTTP/1.0\r\n"
           "Content-Length: %zu\r\n"
           "Content-Type: text/plain\r\n"
           "\r\n",
           my_id_, peer_id, message.length());
  onconnect_data_ = headers;
  onconnect_data_ += message;
  return ConnectControlSocket();
}

bool PeerConnectionClient::SendHangUp(int peer_id) {
  return SendToPeer(peer_id, kByeMessage);
}

bool PeerConnectionClient::IsSendingMessage() {
  return state_ == CONNECTED &&
         control_socket_->GetState() != rtc::Socket::CS_CLOSED;
}

bool PeerConnectionClient::SignOut() {
  if (state_ == NOT_CONNECTED || state_ == SIGNING_OUT)
    return true;

  if (hanging_get_->GetState() != rtc::Socket::CS_CLOSED)
    hanging_get_->Close();

  if (control_socket_->GetState() == rtc::Socket::CS_CLOSED) {
    state_ = SIGNING_OUT;

    if (my_id_ != -1) {
      char buffer[1024];
      snprintf(buffer, sizeof(buffer),
               "GET /sign_out?peer_id=%i HTTP/1.0\r\n\r\n", my_id_);
      onconnect_data_ = buffer;
      return ConnectControlSocket(); // send sign_out signaling.
    } else { 
      // Can occur if the app is closed before we finish connecting.
      return true;
    }
  } else {
    // if control_socket_ is sending other signaling messages, 
    // then it cannot send sign_out immediately.    
    state_ = SIGNING_OUT_WAITING; // mark as waiting sign_out
  }

  return true;
}

void PeerConnectionClient::Close() {
  control_socket_->Close();
  hanging_get_->Close();
  onconnect_data_.clear();
  peers_.clear();
  resolver_.reset();
  my_id_ = -1;
  state_ = NOT_CONNECTED;
}

// This is an async function, after the connection succeeds, OnConnect() will be triggered.
bool PeerConnectionClient::ConnectControlSocket() {
  RTC_DCHECK(control_socket_->GetState() == rtc::Socket::CS_CLOSED);

  // send a connection request to the server
  int err = control_socket_->Connect(server_address_);
  if (err == SOCKET_ERROR) {
    Close();
    return false;
  }
  return true;
}

void PeerConnectionClient::OnConnect(rtc::Socket* socket) {
  RTC_DCHECK(!onconnect_data_.empty());
  // onconnect_data_ is the sign_in request
  size_t sent = socket->Send(onconnect_data_.c_str(), onconnect_data_.length());
  RTC_DCHECK(sent == onconnect_data_.length());
  onconnect_data_.clear();
}

// After hanging_get_ logs in successfully, this function will be triggered. 
// In this function, a wait signal will be sent. When the signaling server needs 
// to actively send a message to the client, it will send the corresponding 
// message as a response to this signal.
void PeerConnectionClient::OnHangingGetConnect(rtc::Socket* socket) {
  char buffer[1024];
  snprintf(buffer, sizeof(buffer), "GET /wait?peer_id=%i HTTP/1.0\r\n\r\n",
           my_id_);
  int len = static_cast<int>(strlen(buffer));
  int sent = socket->Send(buffer, len);
  RTC_DCHECK(sent == len);
}

void PeerConnectionClient::OnMessageFromPeer(int peer_id,
                                             const std::string& message) {
  // if it is a BYE message, remove from peer_list.                                            
  if (message.length() == (sizeof(kByeMessage) - 1) &&
      message.compare(kByeMessage) == 0) {
    callback_->OnPeerDisconnected(peer_id);
  } else {
    // offer、answer、candidate messages need to be handled by Conductor::OnMessageFromPeer()
    callback_->OnMessageFromPeer(peer_id, message);
  }
}

bool PeerConnectionClient::GetHeaderValue(const std::string& data,
                                          size_t eoh,
                                          const char* header_pattern,
                                          size_t* value) {
  RTC_DCHECK(value != NULL);
  size_t found = data.find(header_pattern);
  if (found != std::string::npos && found < eoh) {
    *value = atoi(&data[found + strlen(header_pattern)]);
    return true;
  }
  return false;
}

bool PeerConnectionClient::GetHeaderValue(const std::string& data,
                                          size_t eoh,
                                          const char* header_pattern,
                                          std::string* value) {
  RTC_DCHECK(value != NULL);
  size_t found = data.find(header_pattern);
  if (found != std::string::npos && found < eoh) {
    size_t begin = found + strlen(header_pattern);
    size_t end = data.find("\r\n", begin);
    if (end == std::string::npos)
      end = eoh;
    value->assign(data.substr(begin, end - begin));
    return true;
  }
  return false;
}

bool PeerConnectionClient::ReadIntoBuffer(rtc::Socket* socket,
                                          std::string* data,
                                          size_t* content_length) {
  char buffer[0xffff];
  // receive from socket, and append to data
  do {
    int bytes = socket->Recv(buffer, sizeof(buffer), nullptr);
    if (bytes <= 0)
      break;
    data->append(buffer, bytes);
  } while (true);

  bool ret = false;
  size_t i = data->find("\r\n\r\n"); // find the end of the header
  if (i != std::string::npos) {
    RTC_LOG(LS_INFO) << "Headers received";

    // read Content-Length field from the http header
    if (GetHeaderValue(*data, i, "\r\nContent-Length: ", content_length)) {
      // i point to \r\n\r\n, i+4+content_length is the size of the response
      size_t total_response_size = (i + 4) + *content_length;
      if (data->length() >= total_response_size) {
        ret = true;
        std::string should_close;
        const char kConnection[] = "\r\nConnection: ";
        // read Connection field from the http header, and if it is "close"
        if (GetHeaderValue(*data, i, kConnection, &should_close) &&
            should_close.compare("close") == 0) {
          socket->Close(); 
          // Since we closed the socket, there was no notification delivered
          // to us.  Compensate by letting ourselves know.
          OnClose(socket, 0); // handle both control_socket_ and hanging_get_ close
        }
      } else {
        // We haven't received everything.  Just continue to accept data.
      }
    } else {
      RTC_LOG(LS_ERROR) << "No content length field specified by the server.";
    }
  }
  return ret;
}

// When the signaling server send sign_in response back, this function will be triggered.
void PeerConnectionClient::OnRead(rtc::Socket* socket) {
  size_t content_length = 0;
  // read the response to control_data_ buffer
  if (ReadIntoBuffer(socket, &control_data_, &content_length)) {
    size_t peer_id = 0, eoh = 0;

    // Check response status, and obtain peer_id
    bool ok =
        ParseServerResponse(control_data_, content_length, &peer_id, &eoh);
    if (ok) {

      // my_id_ is the self peer_id, which is assigned by the signaling server. 
      // Its value is -1 before assigned.
      if (my_id_ == -1) {
        // First response.  Let's store our server assigned ID.
        RTC_DCHECK(state_ == SIGNING_IN);
        my_id_ = static_cast<int>(peer_id);
        RTC_DCHECK(my_id_ != -1);

        // The body of the response will be a list of already connected peers.
        if (content_length) { // if the response body is not empty
          // point to the beginning of the body
          size_t pos = eoh + 4;
          // parse the body
          while (pos < control_data_.size()) {
            size_t eol = control_data_.find('\n', pos);
            if (eol == std::string::npos)
              break;
            int id = 0;
            std::string name;
            bool connected;

            // parse one entry in the body
            if (ParseEntry(control_data_.substr(pos, eol - pos), &name, &id,
                           &connected) &&
                id != my_id_) {
              // if not myself, add the peer to the peers_ map
              peers_[id] = name;
              // inform the conductor the peer's id and name, e.g. hp@DESKTOP-740K5HL,2,1
              // response format: peer's name, server assigned peer_id, if it is signed in
              // OnPeerConnected() will display peer's info to the peer list ui
              callback_->OnPeerConnected(id, name);
            }

            // point to next entry
            pos = eol + 1;
          }
        }
        RTC_DCHECK(is_connected());
        callback_->OnSignedIn(); // inform the conductor that the sign_in is successful
      } else if (state_ == SIGNING_OUT) {
        Close();
        callback_->OnDisconnected();
      } else if (state_ == SIGNING_OUT_WAITING) {
        // After the control_socket_ finishes processing a signal, 
        // it will check if it is in the SIGNING_OUT_WAITING state.
        SignOut();
      }
    }

    control_data_.clear();

    if (state_ == SIGNING_IN) {
      RTC_DCHECK(hanging_get_->GetState() == rtc::Socket::CS_CLOSED);
      state_ = CONNECTED;
      // hanging_get_ socket connects to the signaling server to wait for messages
      hanging_get_->Connect(server_address_);
    }
  }
}

// At the callee side, when it receives an offer from the server, it call this function.
void PeerConnectionClient::OnHangingGetRead(rtc::Socket* socket) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  size_t content_length = 0;
  if (ReadIntoBuffer(socket, &notification_data_, &content_length)) {
    size_t peer_id = 0, eoh = 0;
    // parse peer_id from the response
    bool ok =
        ParseServerResponse(notification_data_, content_length, &peer_id, &eoh);

    if (ok) {
      // Store the position where the body begins.
      size_t pos = eoh + 4;

      if (my_id_ == static_cast<int>(peer_id)) {
        // A notification about a new member or a member that just disconnected.
        int id = 0;
        std::string name;
        bool connected = false;
        if (ParseEntry(notification_data_.substr(pos), &name, &id,
                       &connected)) {
          if (connected) {   // if a peer is connected
            peers_[id] = name;
            callback_->OnPeerConnected(id, name);
          } else {           // if a peer is disconnected, update peer_list
            peers_.erase(id);
            callback_->OnPeerDisconnected(id); // inform the conductor
          }
        }
      } else {
        // A message from the signaling server forwarding offer, answer, candidate, 
        // and bye information from other clients.
        // call onMessageFromPeer() to handle the message
        OnMessageFromPeer(static_cast<int>(peer_id),
                          notification_data_.substr(pos));
      }
    }

    notification_data_.clear();
  }

  if (hanging_get_->GetState() == rtc::Socket::CS_CLOSED &&
      state_ == CONNECTED) {
    hanging_get_->Connect(server_address_);
  }
}

bool PeerConnectionClient::ParseEntry(const std::string& entry,
                                      std::string* name,
                                      int* id,
                                      bool* connected) {
  RTC_DCHECK(name != NULL);
  RTC_DCHECK(id != NULL);
  RTC_DCHECK(connected != NULL);
  RTC_DCHECK(!entry.empty());

  *connected = false;
  size_t separator = entry.find(',');
  if (separator != std::string::npos) {
    *id = atoi(&entry[separator + 1]);
    name->assign(entry.substr(0, separator));
    separator = entry.find(',', separator + 1);
    if (separator != std::string::npos) {
      *connected = atoi(&entry[separator + 1]) ? true : false;
    }
  }
  return !name->empty();
}

int PeerConnectionClient::GetResponseStatus(const std::string& response) {
  int status = -1;
  size_t pos = response.find(' ');
  if (pos != std::string::npos)
    status = atoi(&response[pos + 1]);
  return status;
}

bool PeerConnectionClient::ParseServerResponse(const std::string& response,
                                               size_t content_length,
                                               size_t* peer_id,
                                               size_t* eoh) {
  // get the response status                                              
  int status = GetResponseStatus(response.c_str());
  if (status != 200) {  // if the status code is not 200, it means the server returns an error
    RTC_LOG(LS_ERROR) << "Received error from server";
    Close();
    callback_->OnDisconnected();
    return false;
  }

  // find the end of the header
  *eoh = response.find("\r\n\r\n"); 
  RTC_DCHECK(*eoh != std::string::npos);
  if (*eoh == std::string::npos)
    return false;

  *peer_id = -1;

  // See comment in peer_channel.cc for why we use the Pragma header.
  // Pragma is used to pass the server assigned peer_id.
  GetHeaderValue(response, *eoh, "\r\nPragma: ", peer_id);

  return true;
}

void PeerConnectionClient::OnClose(rtc::Socket* socket, int err) {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  socket->Close();

#ifdef WIN32
  if (err != WSAECONNREFUSED) { 
#else
  if (err != ECONNREFUSED) {  
#endif
    if (socket == hanging_get_.get()) {   // if it is the hanging_get_ socket
      if (state_ == CONNECTED) {
        hanging_get_->Close();            // close the socket
        hanging_get_->Connect(server_address_);  // begin a new connection
      }
    } else {                              // if it is the control_socket_ socket
      // inform the conductor that the message is sent, 
      // continue to handle other message sending
      callback_->OnMessageSent(err);      
    }
  } else { // if not able to connect to signaling server
    if (socket == control_socket_.get()) {
      RTC_LOG(LS_WARNING) << "Connection refused; retrying in 2 seconds";
      // if the connection is refused, retry in 2 seconds
      rtc::Thread::Current()->PostDelayedTask(
          SafeTask(safety_.flag(), [this] { DoConnect(); }), kReconnectDelay);
    } else { // if the hanging_get, close the socket
      Close();
      callback_->OnDisconnected();  // inform the conductor that the connection is closed 
    }
  }
}

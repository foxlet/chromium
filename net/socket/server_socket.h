// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_SERVER_SOCKET_H_
#define NET_SOCKET_SERVER_SOCKET_H_

#include <string>

#include "base/memory/scoped_ptr.h"
#include "net/base/completion_callback.h"
#include "net/base/net_export.h"

namespace net {

class IPEndPoint;
class StreamSocket;

class NET_EXPORT ServerSocket {
 public:
  ServerSocket();
  virtual ~ServerSocket();

  // Binds the socket and starts listening. Destroy the socket to stop
  // listening.
  virtual int Listen(const net::IPEndPoint& address, int backlog) = 0;

  // Binds the socket with address and port, and starts listening. It expects
  // a valid IPv4 or IPv6 address. Otherwise, it returns ERR_ADDRESS_INVALID.
  // Subclasses may override this function if |address_string| is in a different
  // format, for example, unix domain socket path.
  virtual int ListenWithAddressAndPort(const std::string& address_string,
                                       int port,
                                       int backlog);

  // Gets current address the socket is bound to.
  virtual int GetLocalAddress(IPEndPoint* address) const = 0;

  // Accepts connection. Callback is called when new connection is
  // accepted.
  virtual int Accept(scoped_ptr<StreamSocket>* socket,
                     const CompletionCallback& callback) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(ServerSocket);
};

}  // namespace net

#endif  // NET_SOCKET_SERVER_SOCKET_H_

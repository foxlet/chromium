// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_QUIC_QUIC_CRYPTO_CLIENT_STREAM_H_
#define NET_QUIC_QUIC_CRYPTO_CLIENT_STREAM_H_

#include <string>

#include "net/quic/crypto/channel_id.h"
#include "net/quic/crypto/proof_verifier.h"
#include "net/quic/crypto/quic_crypto_client_config.h"
#include "net/quic/quic_config.h"
#include "net/quic/quic_crypto_stream.h"
#include "net/quic/quic_server_id.h"

namespace net {

class QuicClientSessionBase;

namespace test {
class CryptoTestUtils;
class QuicClientSessionPeer;
}  // namespace test

class NET_EXPORT_PRIVATE QuicCryptoClientStream : public QuicCryptoStream {
 public:
  QuicCryptoClientStream(const QuicServerId& server_id,
                         QuicClientSessionBase* session,
                         ProofVerifyContext* verify_context,
                         QuicCryptoClientConfig* crypto_config);
  virtual ~QuicCryptoClientStream();

  // CryptoFramerVisitorInterface implementation
  virtual void OnHandshakeMessage(
      const CryptoHandshakeMessage& message) OVERRIDE;

  // Performs a crypto handshake with the server. Returns true if the crypto
  // handshake is started successfully.
  // TODO(agl): this should probably return void.
  virtual bool CryptoConnect();

  // num_sent_client_hellos returns the number of client hello messages that
  // have been sent. If the handshake has completed then this is one greater
  // than the number of round-trips needed for the handshake.
  int num_sent_client_hellos() const;

  // Returns true if a channel ID was sent on this connection.
  bool WasChannelIDSent() const;

 private:
  // ChannelIDSourceCallbackImpl is passed as the callback method to
  // GetChannelIDKey. The ChannelIDSource calls this class with the result of
  // channel ID lookup when lookup is performed asynchronously.
  class ChannelIDSourceCallbackImpl : public ChannelIDSourceCallback {
   public:
    explicit ChannelIDSourceCallbackImpl(QuicCryptoClientStream* stream);
    virtual ~ChannelIDSourceCallbackImpl();

    // ChannelIDSourceCallback interface.
    virtual void Run(scoped_ptr<ChannelIDKey>* channel_id_key) OVERRIDE;

    // Cancel causes any future callbacks to be ignored. It must be called on
    // the same thread as the callback will be made on.
    void Cancel();

   private:
    QuicCryptoClientStream* stream_;
  };

  // ProofVerifierCallbackImpl is passed as the callback method to VerifyProof.
  // The ProofVerifier calls this class with the result of proof verification
  // when verification is performed asynchronously.
  class ProofVerifierCallbackImpl : public ProofVerifierCallback {
   public:
    explicit ProofVerifierCallbackImpl(QuicCryptoClientStream* stream);
    virtual ~ProofVerifierCallbackImpl();

    // ProofVerifierCallback interface.
    virtual void Run(bool ok,
                     const string& error_details,
                     scoped_ptr<ProofVerifyDetails>* details) OVERRIDE;

    // Cancel causes any future callbacks to be ignored. It must be called on
    // the same thread as the callback will be made on.
    void Cancel();

   private:
    QuicCryptoClientStream* stream_;
  };

  friend class test::CryptoTestUtils;
  friend class test::QuicClientSessionPeer;

  enum State {
    STATE_IDLE,
    STATE_INITIALIZE,
    STATE_SEND_CHLO,
    STATE_RECV_REJ,
    STATE_VERIFY_PROOF,
    STATE_VERIFY_PROOF_COMPLETE,
    STATE_GET_CHANNEL_ID,
    STATE_GET_CHANNEL_ID_COMPLETE,
    STATE_RECV_SHLO,
  };

  // DoHandshakeLoop performs a step of the handshake state machine. Note that
  // |in| may be NULL if the call did not result from a received message.
  void DoHandshakeLoop(const CryptoHandshakeMessage* in);

  // Called to set the proof of |cached| valid.  Also invokes the session's
  // OnProofValid() method.
  void SetCachedProofValid(QuicCryptoClientConfig::CachedState* cached);

  // Returns true if the server crypto config in |cached| requires a ChannelID
  // and the client config settings also allow sending a ChannelID.
  bool RequiresChannelID(QuicCryptoClientConfig::CachedState* cached);

  QuicClientSessionBase* client_session();

  State next_state_;
  // num_client_hellos_ contains the number of client hello messages that this
  // connection has sent.
  int num_client_hellos_;

  QuicCryptoClientConfig* const crypto_config_;

  // Client's connection nonce (4-byte timestamp + 28 random bytes)
  std::string nonce_;
  // Server's (hostname, port, is_https, privacy_mode) tuple.
  const QuicServerId server_id_;

  // Generation counter from QuicCryptoClientConfig's CachedState.
  uint64 generation_counter_;

  // channel_id_source_callback_ contains the callback object that we passed
  // to an asynchronous channel ID lookup. The ChannelIDSource owns this
  // object.
  ChannelIDSourceCallbackImpl* channel_id_source_callback_;

  // These members are used to store the result of an asynchronous channel ID
  // lookup. These members must not be used after
  // STATE_GET_CHANNEL_ID_COMPLETE.
  scoped_ptr<ChannelIDKey> channel_id_key_;

  // verify_context_ contains the context object that we pass to asynchronous
  // proof verifications.
  scoped_ptr<ProofVerifyContext> verify_context_;

  // proof_verify_callback_ contains the callback object that we passed to an
  // asynchronous proof verification. The ProofVerifier owns this object.
  ProofVerifierCallbackImpl* proof_verify_callback_;

  // These members are used to store the result of an asynchronous proof
  // verification. These members must not be used after
  // STATE_VERIFY_PROOF_COMPLETE.
  bool verify_ok_;
  string verify_error_details_;
  scoped_ptr<ProofVerifyDetails> verify_details_;

  DISALLOW_COPY_AND_ASSIGN(QuicCryptoClientStream);
};

}  // namespace net

#endif  // NET_QUIC_QUIC_CRYPTO_CLIENT_STREAM_H_

// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <list>
#include <memory>
#include "src/AllowanceSemaphore.h"
#include "src/Common.h"
#include "src/DuplexConnection.h"
#include "src/Executor.h"
#include "src/Frame.h"
#include "src/FrameProcessor.h"
#include "src/FrameSerializer.h"
#include "src/Payload.h"
#include "src/ReactiveStreamsCompat.h"
#include "src/StreamsFactory.h"

namespace reactivesocket {

class StreamAutomatonBase;
class ClientResumeStatusCallback;
class ConnectionAutomaton;
class DuplexConnection;
class Frame_ERROR;
class FrameTransport;
class KeepaliveTimer;
class RequestHandler;
class Stats;
class StreamState;

enum class FrameType : uint16_t;

/// Creates, registers and spins up responder for provided new stream ID and
/// serialised frame.
///
/// It is a responsibility of this strategy to register the responder with the
/// connection automaton and provide it with the initial frame if needed.
/// Returns true if the responder has been created successfully, false if the
/// frame cannot start a new stream, in which case the frame (passed by a
/// mutable referece) must not be modified.
using ConnectionLevelFrameHandler =
    std::function<void(std::unique_ptr<folly::IOBuf>)>;

using ResumeListener = std::function<std::shared_ptr<StreamState>(
    const ResumeIdentificationToken& token)>;

class FrameSink {
 public:
  virtual ~FrameSink() = default;

  /// Terminates underlying connection sending the error frame
  /// on the connection.
  ///
  /// This may synchronously deliver terminal signals to all
  /// StreamAutomatonBase attached to this ConnectionAutomaton.
  virtual void disconnectOrCloseWithError(Frame_ERROR&& error) = 0;

  virtual void sendKeepalive() = 0;
};

/// Handles connection-level frames and (de)multiplexes streams.
///
/// Instances of this class should be accessed and managed via shared_ptr,
/// instead of the pattern reflected in MemoryMixin and IntrusiveDeleter.
/// The reason why such a simple memory management story is possible lies in the
/// fact that there is no request(n)-based flow control between stream
/// automata and ConnectionAutomaton.
class ConnectionAutomaton final
    : public FrameSink,
      public FrameProcessor,
      public ExecutorBase,
      public std::enable_shared_from_this<ConnectionAutomaton> {
 public:
  ConnectionAutomaton(
      folly::Executor& executor,
      ConnectionLevelFrameHandler connectionLevelFrameHandler,
      std::shared_ptr<RequestHandler> requestHandler,
      ResumeListener resumeListener,
      Stats& stats,
      std::unique_ptr<KeepaliveTimer> keepaliveTimer_,
      ReactiveSocketMode mode);

  void closeWithError(Frame_ERROR&& error);
  void disconnectOrCloseWithError(Frame_ERROR&& error) override;

  /// Kicks off connection procedure.
  ///
  /// May result, depending on the implementation of the DuplexConnection, in
  /// processing of one or more frames.
  void connect(std::shared_ptr<FrameTransport>, bool sendingPendingFrames);

  /// Disconnects DuplexConnection from the automaton.
  /// Existing streams will stay intact.
  void disconnect(folly::exception_wrapper ex);

  /// Terminates underlying connection.
  ///
  /// This may synchronously deliver terminal signals to all
  /// StreamAutomatonBase attached to this ConnectionAutomaton.
  void close(folly::exception_wrapper, StreamCompletionSignal);

  std::shared_ptr<FrameTransport> detachFrameTransport();

  /// Terminate underlying connection and connect new connection
  void reconnect(
      std::shared_ptr<FrameTransport>,
      std::unique_ptr<ClientResumeStatusCallback>);

  ~ConnectionAutomaton();

  /// @{
  /// A contract exposed to StreamAutomatonBase, modelled after Subscriber
  /// and Subscription contracts, while omitting flow control related signals.

  /// Adds a stream automaton to the connection.
  ///
  /// This signal corresponds to Subscriber::onSubscribe.
  ///
  /// No frames will be issued as a result of this call. Stream automaton
  /// must take care of writing appropriate frames to the connection, using
  /// ::writeFrame after calling this method.
  void addStream(
      StreamId streamId,
      std::shared_ptr<StreamAutomatonBase> automaton);

  /// Indicates that the stream should be removed from the connection.
  ///
  /// No frames will be issued as a result of this call. Stream automaton
  /// must take care of writing appropriate frames to the connection, using
  /// ::writeFrame, prior to calling this method.
  ///
  /// This signal corresponds to Subscriber::{onComplete,onError} and
  /// Subscription::cancel.
  /// Per ReactiveStreams specification:
  /// 1. no other signal can be delivered during or after this one,
  /// 2. "unsubscribe handshake" guarantees that the signal will be delivered
  ///   at least once, even if the automaton initiated stream closure,
  /// 3. per "unsubscribe handshake", the automaton must deliver corresponding
  ///   terminal signal to the connection.
  ///
  /// Additionally, in order to simplify implementation of stream automaton:
  /// 4. the signal bound with a particular StreamId is idempotent and may be
  ///   delivered multiple times as long as the caller holds shared_ptr to
  ///   ConnectionAutomaton.
  void endStream(StreamId streamId, StreamCompletionSignal signal);

  void useStreamState(std::shared_ptr<StreamState> streamState);
  /// @}

  void sendKeepalive() override;

  void setResumable(bool resumable);
  Frame_RESUME createResumeFrame(const ResumeIdentificationToken& token) const;

  bool isPositionAvailable(ResumePosition position);
  //  ResumePosition positionDifference(ResumePosition position);

  void outputFrameOrEnqueue(std::unique_ptr<folly::IOBuf> frame);

  template <typename TFrame>
  bool deserializeFrameOrError(
      TFrame& frame,
      std::unique_ptr<folly::IOBuf> payload) {
    if (frameSerializer().deserializeFrom(frame, std::move(payload))) {
      return true;
    } else {
      closeWithError(Frame_ERROR::invalidFrame());
      return false;
    }
  }

  template <typename TFrame>
  bool deserializeFrameOrError(
      bool resumable,
      TFrame& frame,
      std::unique_ptr<folly::IOBuf> payload) {
    if (frameSerializer().deserializeFrom(
            frame, std::move(payload), resumable)) {
      return true;
    } else {
      closeWithError(Frame_ERROR::invalidFrame());
      return false;
    }
  }

  bool resumeFromPositionOrClose(ResumePosition position);

  void addConnectedListener(std::function<void()> listener);
  void addDisconnectedListener(ErrorCallback listener);
  void addClosedListener(ErrorCallback listener);

  uint32_t getKeepaliveTime() const;
  bool isDisconnectedOrClosed() const;
  bool isClosed() const;

  DuplexConnection* duplexConnection() const;
  StreamsFactory& streamsFactory() {
    return streamsFactory_;
  }

  FrameSerializer& frameSerializer() const;
  void setFrameSerializer(std::unique_ptr<FrameSerializer>);

  Stats& stats() {
    return stats_;
  }

 private:
  /// Performs the same actions as ::endStream without propagating closure
  /// signal to the underlying connection.
  ///
  /// The call is idempotent and returns false iff a stream has not been found.
  bool endStreamInternal(StreamId streamId, StreamCompletionSignal signal);

  /// @{
  /// FrameProcessor methods are implemented with ExecutorBase and automatic
  /// marshaling
  /// onto the right executor to allow DuplexConnection living on a different
  /// executor
  /// and calling into ConnectionAutomaton.
  void processFrame(std::unique_ptr<folly::IOBuf>) override;
  void onTerminal(folly::exception_wrapper) override;

  void processFrameImpl(std::unique_ptr<folly::IOBuf>);
  void onTerminalImpl(folly::exception_wrapper);
  /// @}

  void onConnectionFrame(std::unique_ptr<folly::IOBuf>);
  void handleUnknownStream(
      StreamId streamId,
      std::unique_ptr<folly::IOBuf> frame);

  void closeStreams(StreamCompletionSignal);
  void closeFrameTransport(
      folly::exception_wrapper,
      StreamCompletionSignal signal);

  void resumeFromPosition(ResumePosition position);
  void outputFrame(std::unique_ptr<folly::IOBuf>);

  void debugCheckCorrectExecutor() const;

  void pauseStreams();
  void resumeStreams();

  ConnectionLevelFrameHandler connectionLevelFrameHandler_;

  std::shared_ptr<StreamState> streamState_;
  std::shared_ptr<RequestHandler> requestHandler_;
  std::shared_ptr<FrameTransport> frameTransport_;
  std::unique_ptr<FrameSerializer> frameSerializer_;

  Stats& stats_;
  ReactiveSocketMode mode_;
  bool isResumable_{false};
  bool remoteResumeable_{false};
  bool isClosed_{false};

  std::list<std::function<void()>> onConnectListeners_;
  std::list<ErrorCallback> onDisconnectListeners_;
  std::list<ErrorCallback> onCloseListeners_;

  ResumeListener resumeListener_;
  const std::unique_ptr<KeepaliveTimer> keepaliveTimer_;

  std::unique_ptr<ClientResumeStatusCallback> resumeCallback_;
  // TODO: this is a temporary hack before stats_ is turned into shared_ptr
  bool isClosing_{false};

  StreamsFactory streamsFactory_;
};
}

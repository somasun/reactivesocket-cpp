// Copyright 2004-present Facebook. All Rights Reserved.

#include "src/automata/StreamSubscriptionRequesterBase.h"
#include <folly/MoveWrapper.h>

namespace reactivesocket {

void StreamSubscriptionRequesterBase::requestImpl(size_t n) noexcept {
  if (n == 0) {
    return;
  }

  switch (state_) {
    case State::NEW: {
      state_ = State::REQUESTED;
      // FIXME: find a root cause of this asymmetry; the problem here is that
      // the Base::request might be delivered after the whole thing is shut
      // down, if one uses InlineConnection.
      auto initialN =
          n > Frame_REQUEST_N::kMaxRequestN ? Frame_REQUEST_N::kMaxRequestN : n;
      auto remainingN = n > Frame_REQUEST_N::kMaxRequestN
          ? n - Frame_REQUEST_N::kMaxRequestN
          : 0;

      // Send as much as possible with the initial request.
      CHECK_GE(Frame_REQUEST_N::kMaxRequestN, initialN);

      // We must inform ConsumerMixin about an implicit allowance we have
      // requested from the remote end.
      addImplicitAllowance(initialN);
      sendRequestFrame(
          FrameFlags_REQN_PRESENT, initialN, std::move(initialPayload_));

      // Pump the remaining allowance into the ConsumerMixin _after_ sending the
      // initial request.
      if (remainingN) {
        Base::generateRequest(remainingN);
      }
    } break;
    case State::REQUESTED:
      Base::generateRequest(n);
      break;
    case State::CLOSED:
      break;
  }
}

void StreamSubscriptionRequesterBase::cancelImpl() noexcept {
  switch (state_) {
    case State::NEW:
      state_ = State::CLOSED;
      connection_->endStream(streamId_, StreamCompletionSignal::GRACEFUL);
      break;
    case State::REQUESTED: {
      state_ = State::CLOSED;
      auto frame = Frame_CANCEL(streamId_);
      connection_->outputFrameOrEnqueue(
          connection_->frameSerializer().serializeOut(std::move(frame)));
      connection_->endStream(streamId_, StreamCompletionSignal::GRACEFUL);
    } break;
    case State::CLOSED:
      break;
  }
}

void StreamSubscriptionRequesterBase::endStream(StreamCompletionSignal signal) {
  switch (state_) {
    case State::NEW:
    case State::REQUESTED:
      // Spontaneous ::endStream signal means an error.
      DCHECK(StreamCompletionSignal::GRACEFUL != signal);
      state_ = State::CLOSED;
      break;
    case State::CLOSED:
      break;
  }
  Base::endStream(signal);
}

void StreamSubscriptionRequesterBase::onNextFrame(Frame_RESPONSE&& frame) {
  bool end = false;
  switch (state_) {
    case State::NEW:
      // Cannot receive a frame before sending the initial request.
      CHECK(false);
      break;
    case State::REQUESTED:
      if (frame.header_.flags_ & FrameFlags_COMPLETE) {
        state_ = State::CLOSED;
        end = true;
      }
      break;
    case State::CLOSED:
      break;
  }

  processPayload(std::move(frame.payload_));

  if (end) {
    connection_->endStream(streamId_, StreamCompletionSignal::GRACEFUL);
  }
}

void StreamSubscriptionRequesterBase::onNextFrame(Frame_ERROR&& frame) {
  switch (state_) {
    case State::NEW:
      // Cannot receive a frame before sending the initial request.
      CHECK(false);
      break;
    case State::REQUESTED:
      state_ = State::CLOSED;
      Base::onError(std::runtime_error(frame.payload_.moveDataToString()));
      connection_->endStream(streamId_, StreamCompletionSignal::ERROR);
      break;
    case State::CLOSED:
      break;
  }
}
}

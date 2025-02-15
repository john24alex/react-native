/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/renderer/core/EventBeat.h>
#include <react/renderer/core/EventListener.h>
#include <react/renderer/core/EventQueue.h>
#include <react/renderer/core/EventQueueProcessor.h>
#include <react/renderer/core/StateUpdate.h>

namespace facebook::react {

struct RawEvent;

/*
 * Represents event-delivery infrastructure.
 * Particular `EventEmitter` clases use this for sending events.
 */
class EventDispatcher {
 public:
  using Shared = std::shared_ptr<const EventDispatcher>;
  using Weak = std::weak_ptr<const EventDispatcher>;

  EventDispatcher(
      const EventQueueProcessor& eventProcessor,
      const EventBeat::Factory& synchonousEventBeatFactory,
      const EventBeat::Factory& asynchronousEventBeatFactory,
      const EventBeat::SharedOwnerBox& ownerBox);

  /*
   * Dispatches a raw event with given priority using event-delivery pipe.
   */
  void dispatchEvent(RawEvent&& rawEvent) const;

  /*
   * Dispatches a raw event with asynchronous batched priority. Before the
   * dispatch we make sure that no other RawEvent of same type and same target
   * is on the queue.
   */
  void dispatchUniqueEvent(RawEvent&& rawEvent) const;

  /*
   * Dispatches a state update with given priority.
   */
  void dispatchStateUpdate(StateUpdate&& stateUpdate) const;

#pragma mark - Event listeners
  /*
   * Adds provided event listener to the event dispatcher.
   */
  void addListener(const std::shared_ptr<const EventListener>& listener) const;

  /*
   * Removes provided event listener to the event dispatcher.
   */
  void removeListener(
      const std::shared_ptr<const EventListener>& listener) const;

 private:
  EventQueue eventQueue_;

  mutable EventListenerContainer eventListeners_;
};

} // namespace facebook::react

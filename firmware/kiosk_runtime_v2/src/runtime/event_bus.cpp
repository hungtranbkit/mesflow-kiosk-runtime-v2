#include "event_bus.h"

namespace kiosk::runtime {

void EventBus::subscribe(Subscriber subscriber) {
  subscribers_.push_back(std::move(subscriber));
}

void EventBus::publish(const LocalEvent& event) {
  // Synchronous fan-out. A subscriber throwing/blocking would stall every
  // other subscriber — acceptable for Phase 0's small, trusted subscriber
  // set (runtime + renderer), revisit if that assumption stops holding.
  for (auto& subscriber : subscribers_) {
    subscriber(event);
  }
}

}  // namespace kiosk::runtime

#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "common/config.h"
#include "tracker/tracker_state.h"

namespace p2p {

// Keeps this tracker's state in step with the other tracker.
//
// Model: both trackers are equal peers. Each accepts client writes at any
// time and pushes its state to the other; neither is a primary, so there
// is no election and no failover step -- either tracker alone can serve
// every command, which is what "keeps working while one is down" needs.
//
// Convergence is by union merge (see TrackerState::merge_from), which
// makes replication idempotent and order-independent. Re-sending the same
// state changes nothing, so a message lost to a dropped connection needs
// no acknowledgement or retry logic: the next push carries it.
//
// The trade-off is that deletions do not replicate -- a leave_group or
// stop_share that happens while the link is down can be resurrected by a
// later merge. Recording tombstones would fix it; see docs/DECISIONS.md.
//
// The thread reconnects on its own, so trackers can be started in either
// order and a tracker that dies can rejoin without operator action.
class PeerLink {
public:
  PeerLink(TrackerState &state, const Endpoint &peer);
  ~PeerLink();

  void start();
  void stop();

  bool connected() const { return connected_.load(); }

private:
  void loop();
  // One connect-sync-push cycle. Returns false when the link drops, so
  // the caller retries.
  bool session();

  TrackerState &state_;
  Endpoint peer_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  unsigned long long last_pushed_version_ = 0;
};

} // namespace p2p

#pragma once
// DNS responder for the setup AP: answers every A query with the AP's address
// (192.168.4.1), so a phone joining the AP opens the setup page by itself.
// Bound to the AP address only; the station's DNS is unaffected.

namespace captive_dns {

// Both are idempotent and safe to call from any task.
void start();
void stop();

}  // namespace captive_dns

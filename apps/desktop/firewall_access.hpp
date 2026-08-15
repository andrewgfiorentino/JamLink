// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace jamlink::desktop {

// A router mapping only carries a packet as far as the PC. Windows Firewall
// then decides whether JamLink receives it, and for an unsigned application run
// out of a ZIP the answer is usually no. Every other check reports success, so
// this has to be detected and repaired in the application rather than left to a
// musician with an Administrator PowerShell prompt.
enum class FirewallRuleState {
    // The firewall is off for the profile in use, so nothing is being blocked.
    NotEnforced,
    // An enabled inbound UDP allow rule exists for this exact executable.
    Allowed,
    // No rule covers this executable.
    Missing,
    // A JamLink rule exists but points at a different executable, which is what
    // an update or a moved folder leaves behind.
    StalePath,
    // The active network is marked Public. Opening a listening port there is a
    // choice the user should make deliberately, so JamLink asks rather than
    // widening the rule itself.
    PublicNetwork,
    // The state could not be determined.
    Unknown
};

struct FirewallAccess final {
    FirewallRuleState state{FirewallRuleState::Unknown};
    QString activeProfile;
    // Rules found carrying the JamLink name, including stale and duplicate
    // entries, so a repair can clean up rather than pile another one on.
    int matchingRules{0};
    int staleRules{0};
};

// Reads current state. Safe to call repeatedly; performs no modification.
[[nodiscard]] FirewallAccess queryFirewallAccess();

// Creates or repairs a narrowly scoped inbound rule for this executable:
// UDP only, inbound only, private and domain profiles, allow. Removes stale
// JamLink rules pointing at other paths so duplicates do not accumulate.
//
// Elevation is requested through the standard Windows UAC prompt. Returns
// false if the user declined it or the change failed; the caller must re-query
// rather than assume success.
[[nodiscard]] bool requestFirewallRule();

} // namespace jamlink::desktop

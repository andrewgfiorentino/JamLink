// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/nat_behaviour.hpp"

namespace jamlink::network {

std::string_view natBehaviourName(NatMappingBehaviour behaviour) noexcept {
    switch (behaviour) {
    case NatMappingBehaviour::NotProbed: return "NotProbed";
    case NatMappingBehaviour::Inconclusive: return "Inconclusive";
    case NatMappingBehaviour::EndpointIndependent: return "EndpointIndependent";
    case NatMappingBehaviour::AddressOrPortDependent: return "AddressOrPortDependent";
    }
    return "Unknown";
}

std::string_view NatAssessment::advice() const noexcept {
    switch (behaviour) {
    case NatMappingBehaviour::NotProbed:
    case NatMappingBehaviour::Inconclusive:
    case NatMappingBehaviour::EndpointIndependent:
        // Nothing to say. Advice on a connection that will work is noise, and
        // advice on a case we could not measure would be a guess.
        return "";
    case NatMappingBehaviour::AddressOrPortDependent:
        // Never "symmetric NAT". The musician cannot act on that, and the one
        // thing they can act on is who creates the invite.
        return "Your internet connection changes the address JamLink is reached "
               "on every time it is used, so an invite you create leads nowhere. "
               "Ask your friend to create the invite instead and join theirs -- "
               "that works from here, and the session is identical either way.";
    }
    return "";
}

NatAssessment classifyNatBehaviour(
    const ObservedMapping& first, const ObservedMapping& second) noexcept {
    NatAssessment result;
    if (!first.observed && !second.observed) {
        result.behaviour = NatMappingBehaviour::NotProbed;
        return result;
    }
    if (!first.observed || !second.observed) {
        // One server answering proves the machine has a public address. It
        // says nothing about whether the port would be the same to a different
        // destination, and that is the whole question.
        result.behaviour = NatMappingBehaviour::Inconclusive;
        return result;
    }
    if (first.port != second.port) {
        result.behaviour = NatMappingBehaviour::AddressOrPortDependent;
        result.portRewritten = true;
        return result;
    }
    if (first.address != second.address) {
        // A pool of external addresses. Rarer than a rewritten port and just as
        // fatal to an invite, because the address in it was only ever this
        // machine's address as far as one server was concerned.
        result.behaviour = NatMappingBehaviour::AddressOrPortDependent;
        return result;
    }
    result.behaviour = NatMappingBehaviour::EndpointIndependent;
    return result;
}

} // namespace jamlink::network

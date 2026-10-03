// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/DiscoveryService.hpp>
#include <FastCache/Cluster/ProvenFleetSummary.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache::Cluster
{

namespace
{
    /// What a throttled line adds when it stands for more than itself.
    /// @param standsFor How many occurrences the line reports, itself included.
    /// @param what What each occurrence is, plural-tolerant: "proof(s)".
    /// @return The suffix, or nothing for one.
    [[nodiscard]] std::string SinceLastReport(std::size_t standsFor, std::string_view what = "proof(s)")
    {
        return standsFor > 1 ? std::format(" ({} such {} since the last report)", standsFor, what) : std::string {};
    }
} // namespace

DiscoveryService::DiscoveryService(core::net::IDatagramSocket& socket,
                                   core::platform::IClock& clock,
                                   ISecureRandom& random,
                                   PeerDirectory& directory,
                                   DiscoveryConfig config,
                                   IFleetSummarySource const& self,
                                   IFleetObserver& fleets,
                                   Consensus::IRaftPeerKeys const& keys,
                                   IMetricsSink& metrics,
                                   ILogger& logger):
    _socket { socket },
    _clock { clock },
    _directory { directory },
    _config { std::move(config) },
    _self { self },
    _fleets { fleets },
    _cookies { random, _config.challengeLifetime },
    _keys { keys },
    _metrics { metrics },
    _logger { logger },
    _answerBudget { DiscoveryWork::Answer, clock.now() },
    _checkBudget { DiscoveryWork::ProofCheck, clock.now() }
{
    // A precondition, not a runtime state: every caller builds one, and a service with none would
    // fail at its first beacon rather than here where the mistake is.
    if (_config.beaconDestinations == nullptr)
        throw std::invalid_argument { "DiscoveryConfig::beaconDestinations is required" };
}

std::string DescribeRefusals(std::span<BeaconRefusal const> refused)
{
    auto text = std::string {};
    for (auto const& refusal: refused)
    {
        if (!text.empty())
            text += "; ";
        text += std::format(
            "{}: {}", FormatHostPort(refusal.destination.host, refusal.destination.port), refusal.error.toString());
    }
    return text;
}

BeaconSendReport DiscoveryService::SendBeacon()
{
    auto summary = AnnounceableSummary();
    if (!summary.has_value())
        return BeaconSendReport { .outcome = BeaconSendOutcome::Withheld, .accepted = {}, .refused = {} };
    auto const destinations = _config.beaconDestinations->Destinations();
    if (destinations.empty())
        return BeaconSendReport { .outcome = BeaconSendOutcome::NoDestination, .accepted = {}, .refused = {} };
    auto const datagram = DiscoveryWire::EncodeBeacon({ .summary = *std::move(summary) });

    // One datagram per destination, and each one's failure is its own: a link that refuses a send
    // does not keep this node from the links that take it. Every destination is recorded as taken
    // or refused, with what the stack said, and the outcome is read from both lists.
    auto report = BeaconSendReport { .outcome = BeaconSendOutcome::AllRefused, .accepted = {}, .refused = {} };
    for (auto const& destination: destinations)
    {
        auto result = _socket.send(datagram, destination);
        if (result.has_value())
            report.accepted.push_back(destination);
        else
            report.refused.push_back(BeaconRefusal { .destination = destination, .error = std::move(result).error() });
    }
    if (report.refused.empty())
        report.outcome = BeaconSendOutcome::Sent;
    else if (!report.accepted.empty())
        report.outcome = BeaconSendOutcome::PartlyRefused;
    return report;
}

std::optional<CompileCacheWire::FleetSummary> DiscoveryService::AnnounceableSummary()
{
    // Cut to what a datagram carries HERE, the one door both the beacon and the proof leave by, so
    // the two never carry different lists and neither carries more than a reader accepts.
    auto summary = CompileCacheWire::WithMembersAtMost(_self.Current(), CompileCacheWire::MaxFleetSummaryMembers);
    auto const onlyHere = EndpointsOnlyThisMachine(summary);
    if (onlyHere.empty())
        return summary;

    // Withheld rather than sent, and said: every peer resolves such a name to ITSELF, so a
    // beacon naming it would send each of them to dial itself, confidently, with no error at
    // either end. The discovery tier stands down before it gets here on a node configured that
    // way; this is the rule held at the only door an announcement passes, whoever built the
    // summary. Throttled, because the beacon interval would repeat it forever.
    if (auto const now = _clock.now(); now >= _nextUnannounceableReport)
    {
        _nextUnannounceableReport = now + UnannounceableReportInterval;
        auto named = std::string {};
        for (auto const endpoint: onlyHere)
            named += std::format("{}{}", named.empty() ? "" : " and ", endpoint);
        _logger.Logf(LogLevel::Warn,
                     "discovery: withheld this node's announcement, because it names {}, which every peer "
                     "resolves to itself",
                     named);
    }
    return std::nullopt;
}

DiscoveryEvent DiscoveryService::IssueChallenge(DiscoveryWire::Beacon const& peer,
                                                core::net::DatagramAddress const& replyTo,
                                                std::size_t beaconBytes)
{
    // Drawn through the randomness seam rather than a local engine, and held by NOBODY: the nonce
    // is a cookie under a key drawn per epoch (`ChallengeCookies`), which the proof echoes and this
    // node recomputes from what the proof claims. So a flood of beacons naming invented ids grows
    // no table here and displaces nobody's challenge; each costs one HMAC and one datagram no
    // larger than itself.
    auto const challenge = _cookies.Issue(_clock.now(), _self.Current().clusterId, peer.summary, replyTo.host);
    if (!challenge.has_value())
    {
        // Withheld rather than issued under a weak key (#1527), and said. The line names no peer:
        // nothing about the peer is wrong, and what it claimed is unproved.
        if (auto const now = _clock.now(); now >= _nextNoNonceReport)
        {
            _nextNoNonceReport = now + NoNonceReportInterval;
            _logger.Logf(LogLevel::Error,
                         "discovery: withheld a challenge, because this node cannot draw the key its challenges "
                         "are made under: {}. No peer can prove the key to this node until it can",
                         challenge.error().ToString());
        }
        return DiscoveryEvent::ChallengeWithheld;
    }

    // Padded to exactly the beacon, and not sent when it cannot fit: the beacon's source address is
    // whatever its sender typed, so a challenge larger than it would be an amplifier aimed at a
    // third party. An honest beacon is as large as its sender's proof, which always fits this
    // node's challenge -- a cluster id is bounded so that it does (`LargestUnpaddedChallenge`) --
    // so only a beacon shorn of its padding is refused here. Counted and not logged, since
    // anything on the segment can provoke it.
    auto const datagram = DiscoveryWire::EncodeChallenge(*challenge, beaconBytes);
    if (!datagram.has_value())
    {
        _metrics.Increment(IMetricsSink::Counter::DiscoveryRepliesWithheld);
        return DiscoveryEvent::ReplyWithheld;
    }

    // Unicast to where the datagram actually came from, not to what it claimed.
    // A beacon that lies about its endpoint should not be able to aim this
    // node's challenges at a third party.
    (void) _socket.send(*datagram, replyTo);
    return DiscoveryEvent::PeerSeen;
}

DiscoveryEvent DiscoveryService::PumpOnce(std::chrono::milliseconds timeout)
{
    auto const received = _socket.receive(timeout);
    if (!received.has_value())
        return received.error() == core::net::DatagramWait::Closed ? DiscoveryEvent::Closed : DiscoveryEvent::Nothing;

    auto const kind = DiscoveryWire::ClassifyDatagram(received->payload);
    if (!kind.has_value())
        return DiscoveryEvent::Ignored;

    switch (*kind)
    {
        case DiscoveryWire::Kind::Beacon: {
            auto const beacon = DiscoveryWire::DecodeBeacon(received->payload);
            if (!beacon.has_value())
                return DiscoveryEvent::Ignored;

            // The directory decides what this beacon is -- our own fleet's peer,
            // another fleet's node, our own, or an identity nothing could record.
            // Only a peer or another fleet is worth a challenge, and a peer it
            // declined is one no membership proposal can ever be generated for.
            // A `switch` without a `default`, so another outcome is a build failure
            // rather than one nobody reports: "deliberately silent" and "somebody
            // forgot" are otherwise the same state.
            switch (_directory.NoteBeacon(beacon->summary))
            {
                // Another fleet is challenged like a peer: a solitary node has to see
                // it to yield, an established one to say so. Its proof is judged by
                // its signature alone and never reaches the roster (`JudgeProof`).
                case BeaconOutcome::Recorded:
                case BeaconOutcome::Foreign:
                    break;

                // Recorded all the same, by displacing the oldest entry nothing vouches for: a
                // bound was reached, which is counted, and the newcomer is challenged like any.
                case BeaconOutcome::RecordedDisplacing:
                case BeaconOutcome::ForeignDisplacing:
                    _metrics.Increment(IMetricsSink::Counter::DiscoveryBeaconsOverBound);
                    break;

                // Ordinary: this node's own beacon comes back on every broadcast, and
                // reporting it would bury the one that is a fault.
                case BeaconOutcome::Self:
                    return DiscoveryEvent::Ignored;

                // A bound, counted where it is reached -- and said, unlike every other bound, because
                // it is reached only when every other fleet remembered has PROVEN its summary. That
                // takes an answering host with a key of its own per slot rather than a spoofed
                // datagram, and it costs that a real fleet arriving now is not seen. Named by the
                // address the beacon came from and nothing it claimed, which is unproved.
                case BeaconOutcome::ForeignTableFull:
                    _metrics.Increment(IMetricsSink::Counter::DiscoveryBeaconsOverBound);
                    if (auto const standsFor = _foreignTableFullReport.Note(_clock.now()); standsFor.has_value())
                        _logger.Logf(LogLevel::Warn,
                                     "discovery: the beacon of another fleet from {} was not recorded, because every "
                                     "one of the {} other fleets this node remembers has proven itself; while they "
                                     "keep beaconing, no further fleet on this segment is seen{}",
                                     FormatHostPort(received->from.host, received->from.port),
                                     MaxForeignFleets,
                                     SinceLastReport(*standsFor, "beacon(s)"));
                    return DiscoveryEvent::Ignored;

                case BeaconOutcome::Unnameable:
                    // Throttled, because one unauthenticated datagram provokes this
                    // and nothing on the segment has to hold the key to send it --
                    // see `UnnameableReportInterval`.
                    //
                    // Reported by the address it came FROM rather than by what it
                    // claimed, for two reasons that happen to agree. The claim is
                    // the thing that is not text, so it is the one part of such a
                    // beacon that cannot be printed at all -- and the address is
                    // what says which machine to go and look at, which is where the
                    // identity was typed.
                    if (auto const now = _clock.now(); now >= _nextUnnameableReport)
                    {
                        _nextUnnameableReport = now + UnnameableReportInterval;
                        _logger.Logf(LogLevel::Warn,
                                     "discovery: the beacon from {} names an id or endpoint this node cannot record "
                                     "as a member -- an empty id, or not valid UTF-8; ignoring",
                                     FormatHostPort(received->from.host, received->from.port));
                    }
                    return DiscoveryEvent::Ignored;
            }

            return IssueChallenge(*beacon, received->from, received->payload.size());
        }

        case DiscoveryWire::Kind::Challenge: {
            auto const challenge = DiscoveryWire::DecodeChallenge(received->payload);
            if (!challenge.has_value())
                return DiscoveryEvent::Ignored;
            return AnswerChallenge(*challenge, received->payload.size(), received->from);
        }

        case DiscoveryWire::Kind::Proof: {
            auto const proof = DiscoveryWire::DecodeProof(received->payload);
            if (!proof.has_value())
                return DiscoveryEvent::Ignored;

            return JudgeProof(*proof, received->from);
        }

        case DiscoveryWire::Kind::Invalid:
            break;
    }

    return DiscoveryEvent::Ignored;
}

DiscoveryEvent DiscoveryService::AnswerChallenge(DiscoveryWire::Challenge const& challenge,
                                                 std::size_t challengeBytes,
                                                 core::net::DatagramAddress const& replyTo)
{
    // Answered whichever cluster asked. A solitary node of another fleet has to see this one to
    // yield to it, and an established one to say two fleets can see each other -- and what the
    // answer signs is this node's public summary, the bytes its beacon already shouts, so it
    // announces nothing a listener did not have.
    auto summary = AnnounceableSummary();
    if (!summary.has_value())
        return DiscoveryEvent::Ignored;

    // Both refusals come BEFORE the signature, which is the cost a flood would otherwise buy, and
    // neither is logged, since anything on the segment can provoke them. The size first, because
    // it is free: the challenge's source address is whatever its sender typed, so a proof larger
    // than the challenge would be an amplifier aimed at a third party. A challenger of this build
    // pads its challenge to this node's beacon, which is as large as this proof. Then the budget,
    // the source host's own before the one every answer shares (`WorkBudget`).
    if (!DiscoveryWire::AnswerFits(DiscoveryWire::ProofDatagramSize(*summary), challengeBytes)
        || _answerBudget.TryTake(replyTo.host, _clock.now()) != WorkGrant::Granted)
    {
        _metrics.Increment(IMetricsSink::Counter::DiscoveryRepliesWithheld);
        return DiscoveryEvent::ReplyWithheld;
    }

    // Signed with this node's OWN key (#178), over a nonce somebody else chose -- so the signature
    // is fresh, names this node's endpoint, and proves nothing to anybody but the node that asked.
    // The nonce rides back beside it: the asker holds no table of what it asked, and recomputes it.
    auto proof = DiscoveryWire::Proof {
        .summary = *std::move(summary), .answers = challenge.nonce, .publicKey = _keys.OwnPublicKey(), .signature = {}
    };
    proof.signature = _keys.SignAsSelf(DiscoveryWire::ProofMessage(challenge, proof.summary, proof.publicKey));
    (void) _socket.send(DiscoveryWire::EncodeProof(proof), replyTo);
    return DiscoveryEvent::ChallengeAnswered;
}

DiscoveryEvent DiscoveryService::JudgeProof(DiscoveryWire::Proof const& proof, core::net::DatagramAddress const& from)
{
    // The CHALLENGE first, then the SIGNATURE, before anything the proof claims is looked up or
    // reported: a proof answers only a challenge this node issued, for the cluster, node and
    // endpoint it names, and until its signature verifies under the key it carries, that key is a
    // claim too. `ChallengeCookies` asks both, in that order, and spends the challenge only once
    // the signature holds. Whichever cluster the proof names. Between the two it asks the check
    // budget -- the source host's, then the shared one -- since a forgery spends no cookie, and
    // without it one live cookie would buy a signature check per datagram.
    auto const now = _clock.now();
    auto proven = _cookies.Verify(now, _self.Current().clusterId, proof, from.host, [this, &from, now] {
        return _checkBudget.TryTake(from.host, now) == WorkGrant::Granted;
    });
    if (!proven.has_value())
        return Refused(proven.error(), from);

    // Another cluster: judged by that signature ALONE and handed to formation. Never asked of the
    // roster -- this cluster's roster says nothing about another's keys, and asking it would only
    // report every foreign node as an unknown key -- and never recorded as authenticated, so
    // nothing can desire it. What the observer holds is POSSESSION of a key, never the truth of
    // the summary (`ProvenFleetSummary`).
    if (proof.summary.clusterId != _self.Current().clusterId)
    {
        // Its slot is now held by an answer rather than a beacon, and no flood of invented ids
        // displaces it until its beacons stop.
        (void) _directory.MarkForeignProven(proof.summary.clusterId);
        _fleets.OnFleetProven(ProvenFleet::FromBeaconProof(*std::move(proven)));
        return DiscoveryEvent::ForeignFleetProven;
    }

    // Then the roster: whose key is this? Only the key the roster holds for this id proves the
    // id. A revoked key is recognised whatever id it claims, because the list is not narrowed to
    // the claim -- the question is who SIGNED, which only the signature answers (`PeerKeys`).
    auto const keys = _keys.KeysOf(proof.summary.nodeId);
    if (keys.live != std::optional { proof.publicKey })
    {
        // Two increments rather than one over a conditional, so each counter is written by a
        // statement that names it -- which is what `counter-attribution` reads to find a writer.
        auto const revoked = std::ranges::contains(keys.revoked, proof.publicKey);
        if (revoked)
            _metrics.Increment(IMetricsSink::Counter::DiscoveryProofsRefusedRevokedKey);
        else
            _metrics.Increment(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey);
        ReportUnacceptedKey(revoked, proof, from);
        return revoked ? DiscoveryEvent::PeerRevokedKey : DiscoveryEvent::PeerUnknownKey;
    }

    if (!_directory.MarkAuthenticated(proof.summary.nodeId, proof.summary.raftEndpoint, proof.publicKey))
        return DiscoveryEvent::ProofRejected;

    // Told as every authenticated reply is: a reply of this node's own fleet is what says the segment
    // is HEARD, so an empty list of other fleets reads as a finding rather than as deafness.
    _fleets.OnFleetProven(ProvenFleet::FromBeaconProof(*std::move(proven)));

    _logger.Logf(LogLevel::Info,
                 "discovery: {} at {} proved the key the cluster holds for it",
                 proof.summary.nodeId,
                 proof.summary.raftEndpoint);
    return DiscoveryEvent::PeerAuthenticated;
}

DiscoveryEvent DiscoveryService::Refused(CookieRefusal refusal, core::net::DatagramAddress const& from)
{
    // A `switch` without a `default`, so a refusal added to `ChallengeCookies` is a build failure
    // here rather than one nobody reports. Each line names the ADDRESS and nothing the proof
    // claimed: none of it verified, so naming it would be printing bytes anybody could have sent.
    switch (refusal)
    {
        // A forgery answered a challenge this node DID issue, so somebody saw it go out: counted,
        // and said once a minute with how many it stands for.
        case CookieRefusal::Forged:
            _metrics.Increment(IMetricsSink::Counter::DiscoveryProofsRefusedForged);
            if (auto const standsFor = _forgedProofReport.Note(_clock.now()); standsFor.has_value())
                _logger.Logf(LogLevel::Warn,
                             "discovery: the proof from {} is not signed by the key it carries; ignoring it{}",
                             FormatHostPort(from.host, from.port),
                             SinceLastReport(*standsFor));
            return DiscoveryEvent::ProofRejected;

        // Answers nothing this node asked -- or answers for another cluster, node or endpoint than
        // the one challenged, or from another host than the challenge went to, which is the same
        // fact, since the cookie binds all four. Said, once a minute: a peer whose beacon and proof
        // disagree about where it answers is misconfigured.
        case CookieRefusal::NotIssued:
            if (auto const standsFor = _unissuedProofReport.Note(_clock.now()); standsFor.has_value())
                _logger.Logf(LogLevel::Warn,
                             "discovery: the proof from {} answers no challenge this node issued -- or answers "
                             "for another cluster, node or endpoint than the one it was asked at, or from another "
                             "host than the challenge was sent to; ignoring it{}",
                             FormatHostPort(from.host, from.port),
                             SinceLastReport(*standsFor));
            return DiscoveryEvent::ProofRejected;

        // Late, or a second copy of an answer already taken: an honest slow peer and a duplicated
        // datagram look exactly like these, and the next beacon round asks again. Neither said.
        case CookieRefusal::Expired:
            return DiscoveryEvent::ProofExpired;
        case CookieRefusal::Replayed:
            return DiscoveryEvent::ProofReplayed;

        // A live cookie, past the check budget: something is sending proofs faster than this node
        // checks them -- forgeries against one challenge, since a forgery spends none. Counted, and
        // said once a minute by the address it came from.
        case CookieRefusal::Unchecked:
            _metrics.Increment(IMetricsSink::Counter::DiscoveryProofChecksWithheld);
            if (auto const standsFor = _uncheckedProofReport.Note(_clock.now()); standsFor.has_value())
                _logger.Logf(LogLevel::Warn,
                             "discovery: the proof from {} was not checked, because the signature-check budget is "
                             "spent -- that host's own share or everybody's; somebody is sending proofs faster "
                             "than this node checks them{}",
                             FormatHostPort(from.host, from.port),
                             SinceLastReport(*standsFor));
            return DiscoveryEvent::ProofUnchecked;

        // A live cookie that has already failed as many checks as one cookie buys: something keeps
        // sending forgeries against one challenge, from wherever. Counted as a check withheld, and
        // said once a minute by the address this one came from; the next beacon round asks again.
        case CookieRefusal::Exhausted:
            _metrics.Increment(IMetricsSink::Counter::DiscoveryProofChecksWithheld);
            if (auto const standsFor = _exhaustedProofReport.Note(_clock.now()); standsFor.has_value())
                _logger.Logf(LogLevel::Warn,
                             "discovery: the proof from {} was not checked, because the challenge it answers has "
                             "already failed {} signature checks; somebody is sending forgeries against one "
                             "challenge{}",
                             FormatHostPort(from.host, from.port),
                             MaxForgeriesPerChallenge,
                             SinceLastReport(*standsFor));
            return DiscoveryEvent::ProofExhausted;
    }
    return DiscoveryEvent::ProofRejected;
}

void DiscoveryService::ReportUnacceptedKey(bool revoked,
                                           DiscoveryWire::Proof const& proof,
                                           core::net::DatagramAddress const& from)
{
    auto const standsFor = _unacceptedKeyReport.Note(_clock.now());
    if (!standsFor.has_value())
        return;

    // The key WHOLE, because it verified: only its holder could have signed this, so naming it
    // is no oracle, and it is exactly what an operator types after `@` to admit the machine --
    // or recognises as the one they removed. The two remedies are opposite, so each is said.
    auto const key = FormatEd25519PublicKey(proof.publicKey);
    auto const remedy = revoked ? std::string { "that machine was removed, and this key is never admitted again" }
                                : std::format("enroll it, or admit it with --cluster-admit={}={}@{}, if it belongs",
                                              proof.summary.nodeId,
                                              proof.summary.raftEndpoint,
                                              key);
    _logger.Logf(LogLevel::Warn,
                 "discovery: {} at {} (from {}) proved the key {}, which the cluster {}. Not desiring it: {}.{}",
                 proof.summary.nodeId,
                 proof.summary.raftEndpoint,
                 FormatHostPort(from.host, from.port),
                 key,
                 revoked ? "has REVOKED" : "does not hold for that id",
                 remedy,
                 SinceLastReport(*standsFor));
}

void DiscoveryService::Maintain()
{
    _directory.ExpireStale();
}

} // namespace FastCache::Cluster

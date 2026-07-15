// ============================================================================
//  sender.cpp  —  Real-time UDP playout: SENDER edge
// ----------------------------------------------------------------------------
//  Pipeline:  harness source --47010--> [SENDER] --47001--> relay --> receiver
//
//  The source hands us one frame every 20 ms on 47010:
//      [ 4-byte big-endian seq | 160-byte payload ]  (164 bytes total)
//  That leg is a direct localhost link (NOT through the hostile relay), so we
//  receive every frame, in order, on time. All loss/jitter/reorder/dup happens
//  on the sender->relay->receiver media leg; our job is to make it survivable
//  *proactively* (no feedback / ARQ).
//
//  WHY NO ACK/RETRANSMIT: a NAK must cross the hostile relay and the resend
//  must cross it back — a full round trip through the network that is dropping
//  packets. At 20 ms/frame that blows the delay budget. We spend bandwidth
//  ahead of time instead.
//
//  WHY NOT "pack frame i AND i-1 in every packet" (full duplication): the
//  bandwidth cap is measured against the RAW payload stream = 160 B/frame
//  (score.py: raw = n*160). Two 160-B payloads/frame = 320 B *before* headers
//  = already 2.0x, so with any header the run is INVALID. Full duplication
//  cannot fit under 2.0x.
//
//  DESIGN — systematic XOR parity FEC, now INTERLEAVED:
//  --------------------------------------------------------------------------
//  For every group of K frames we send K systematic DATA packets plus one FEC
//  packet = XOR of the K payloads. Any single lost packet in the group is
//  rebuilt as the XOR of the rest. Overhead (K=2) ~1.55x, well under 2.0x.
//
//  Interleaving (FEC_STRIDE = S): the K members of a group are NOT adjacent —
//  they are spaced S frames apart (seqs base, base+S, ..., base+(K-1)*S). A
//  burst of consecutive losses shorter than S can hit at most one member of
//  any group, so it stays recoverable. S=1 is the classic contiguous code
//  (lowest latency, best for pure random loss); S>1 buys burst resilience at
//  the cost of ~ (K-1)*S*20 ms of extra recovery latency.
//
//  Wire format on OUR leg (47001, our design):
//      DATA: [u8 type=0][u32 seq BE][160 payload]                    = 165 B
//      FEC : [u8 type=1][u32 base BE][u8 k][u8 stride][160 xor]       = 167 B
//
//  Tunables (env): FEC_K (default 2), FEC_STRIDE (default 1).
//  Constraints honored: native POSIX sockets, std library only, no
//  `using namespace std;`.
// ============================================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint16_t kSourcePort = 47010;  // bind here (source -> us)
constexpr std::uint16_t kRelayPort  = 47001;  // send here (us -> relay)
constexpr std::size_t   kPayload    = 160;
constexpr std::size_t   kHarnessPkt = 4 + kPayload;  // 164

constexpr std::uint8_t  kTypeData = 0;
constexpr std::uint8_t  kTypeFec  = 1;
constexpr std::size_t   kDataPkt  = 1 + 4 + kPayload;          // 165
constexpr std::size_t   kFecPkt   = 1 + 4 + 1 + 1 + kPayload;  // 167

int EnvInt(const char* key, int def) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return def;
    int parsed = std::atoi(v);
    return parsed > 0 ? parsed : def;
}

inline void PutBE32(std::uint8_t* dst, std::uint32_t v) {
    std::uint32_t be = htonl(v);
    std::memcpy(dst, &be, 4);
}

int MakeSocket() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("socket"); std::exit(1); }
    return fd;
}

// One interleave phase: an independent running XOR over its K in-phase frames.
struct Phase {
    std::uint32_t base = 0;   // seq of the first frame accumulated this group
    int count = 0;            // frames accumulated so far (0..K-1)
    std::uint8_t parity[kPayload];
};

}  // namespace

int main() {
    const int kBlock  = EnvInt("FEC_K", 2);       // frames per FEC group
    const int kStride = EnvInt("FEC_STRIDE", 1);  // interleave spacing

    // Inbound: bind 47010 (frames from the source).
    int in_fd = MakeSocket();
    {
        int one = 1;
        ::setsockopt(in_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kSourcePort);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (::bind(in_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
            std::perror("bind 47010");
            return 1;
        }
    }

    // Outbound: relay destination.
    int out_fd = MakeSocket();
    sockaddr_in relay{};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(kRelayPort);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    // One accumulator per interleave phase (phase = seq % stride).
    std::vector<Phase> phases(kStride);

    std::uint8_t in_buf[2048];
    std::uint8_t data_pkt[kDataPkt];
    std::uint8_t fec_pkt[kFecPkt];
    data_pkt[0] = kTypeData;
    fec_pkt[0]  = kTypeFec;

    for (;;) {
        ssize_t n = ::recvfrom(in_fd, in_buf, sizeof in_buf, 0, nullptr, nullptr);
        if (n < static_cast<ssize_t>(kHarnessPkt)) continue;

        std::uint32_t seq_be;
        std::memcpy(&seq_be, in_buf, 4);
        const std::uint32_t seq = ntohl(seq_be);
        const std::uint8_t* payload = in_buf + 4;

        // (1) Emit the systematic DATA packet immediately — never hold a frame.
        std::memcpy(data_pkt + 1, in_buf, 4);            // seq is already BE
        std::memcpy(data_pkt + 5, payload, kPayload);
        ::sendto(out_fd, data_pkt, kDataPkt, 0,
                 reinterpret_cast<sockaddr*>(&relay), sizeof relay);

        // (2) Fold into this frame's interleave phase.
        Phase& ph = phases[seq % static_cast<std::uint32_t>(kStride)];
        if (ph.count == 0) {
            ph.base = seq;
            std::memset(ph.parity, 0, sizeof ph.parity);
        }
        for (std::size_t b = 0; b < kPayload; ++b) ph.parity[b] ^= payload[b];
        ++ph.count;

        // (3) Once a phase has K members, ship its parity (members are
        //     base, base+S, ..., base+(K-1)*S) and reset the phase.
        if (ph.count == kBlock) {
            PutBE32(fec_pkt + 1, ph.base);
            fec_pkt[5] = static_cast<std::uint8_t>(kBlock);
            fec_pkt[6] = static_cast<std::uint8_t>(kStride);
            std::memcpy(fec_pkt + 7, ph.parity, kPayload);
            ::sendto(out_fd, fec_pkt, kFecPkt, 0,
                     reinterpret_cast<sockaddr*>(&relay), sizeof relay);
            ph.count = 0;
        }
    }
    return 0;  // unreachable: harness kills us at end of run
}

// ============================================================================
//  receiver.cpp  —  Real-time UDP playout: RECEIVER edge
// ----------------------------------------------------------------------------
//  Pipeline:  sender --> relay --47002--> [RECEIVER] --47020--> harness player
//
//  The player judges frame i against a hard deadline:
//      deadline(i) = T0 + DELAY_MS/1000 + i*20ms
//  A frame counts only if a packet with the correct payload for seq i arrives
//  on 47020 at or before deadline(i). Early arrival is free.
//
//  Threading (main + dedicated playout, as requested):
//    * Receive thread: sole reader of 47002. Parses our wire format, writes
//      payloads into a sequence-indexed jitter buffer, runs XOR-FEC recovery.
//      Never blocks on output.
//    * Playout thread: walks sequence numbers in order. For each frame p it
//      waits (condition_variable, absolute wall-clock deadline) until the
//      frame is present or its send-by instant, emits it, and advances.
//      In-order + per-frame deadline is self-clocking: a permanently lost
//      frame cannot delay the frames behind it past their own deadlines.
//
//  Forward-as-soon-as-present: the scorer records FIRST arrival and only
//  checks arrival <= deadline, so sending early is strictly safer. We only
//  *wait* while a frame is still missing — buying time for a reordered copy or
//  an FEC reconstruction.
//
//  FEC recovery understands INTERLEAVING: a parity packet protects seqs
//  base, base+stride, ..., base+(k-1)*stride. If exactly one member is
//  missing, it is rebuilt as parity XOR (all present members).
//
//  Constraints honored: native POSIX sockets, std threading only, no
//  `using namespace std;`.
// ============================================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t kRelayPort  = 47002;  // bind here (relay -> us)
constexpr std::uint16_t kPlayerPort = 47020;  // send here (us -> player)
constexpr std::size_t   kPayload    = 160;
constexpr std::size_t   kHarnessPkt = 4 + kPayload;  // 164 to the player

constexpr std::uint8_t  kTypeData = 0;
constexpr std::uint8_t  kTypeFec  = 1;
constexpr std::size_t   kDataPkt  = 1 + 4 + kPayload;          // 165
constexpr std::size_t   kFecPkt   = 1 + 4 + 1 + 1 + kPayload;  // 167

constexpr std::size_t   kCap      = 1u << 15;  // 32768 jitter-buffer slots
constexpr std::size_t   kMask     = kCap - 1;
constexpr double        kSendMarginS = 0.002;  // send <=2 ms before deadline
constexpr std::size_t   kFecRing  = 1024;      // retained parity packets

double EnvDouble(const char* key, double def) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return def;
    return std::atof(v);
}

struct Slot {
    std::uint32_t seq = 0xFFFFFFFFu;
    bool present = false;
    std::uint8_t data[kPayload];
};

// Retained parity: XOR of seqs base, base+stride, ..., base+(k-1)*stride.
struct FecRec {
    std::uint32_t base = 0;
    std::uint8_t k = 0;
    std::uint8_t stride = 1;
    std::uint8_t xorp[kPayload];
    bool used = false;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::vector<Slot> g_buf(kCap);
std::vector<FecRec> g_fec(kFecRing);
std::size_t g_fec_head = 0;

bool StoreLocked(std::uint32_t seq, const std::uint8_t* payload) {
    Slot& s = g_buf[seq & kMask];
    if (s.present && s.seq == seq) return false;  // duplicate: first write wins
    s.seq = seq;
    s.present = true;
    std::memcpy(s.data, payload, kPayload);
    return true;
}

bool IsPresentLocked(std::uint32_t seq) {
    const Slot& s = g_buf[seq & kMask];
    return s.present && s.seq == seq;
}

// Does this parity group protect `seq`? (interleave-aware)
bool CoversLocked(const FecRec& f, std::uint32_t seq) {
    if (f.stride == 0 || seq < f.base) return false;
    std::uint32_t off = seq - f.base;
    return (off % f.stride == 0) && (off / f.stride < f.k);
}

// Rebuild the single missing member of a group, if exactly one is missing.
// Returns the recovered seq, or -1. Must hold g_mu.
long TryRecoverLocked(FecRec& f) {
    if (f.used || f.k == 0 || f.stride == 0) return -1;
    int missing = -1, missing_count = 0;
    for (int j = 0; j < f.k; ++j) {
        std::uint32_t s = f.base + static_cast<std::uint32_t>(j) * f.stride;
        if (!IsPresentLocked(s)) {
            missing = j;
            if (++missing_count > 1) return -1;  // >1 hole: unrecoverable here
        }
    }
    if (missing_count == 0) { f.used = true; return -1; }  // already complete

    std::uint8_t out[kPayload];
    std::memcpy(out, f.xorp, kPayload);
    for (int j = 0; j < f.k; ++j) {
        if (j == missing) continue;
        std::uint32_t s = f.base + static_cast<std::uint32_t>(j) * f.stride;
        const Slot& sl = g_buf[s & kMask];
        for (std::size_t b = 0; b < kPayload; ++b) out[b] ^= sl.data[b];
    }
    f.used = true;
    std::uint32_t rseq = f.base + static_cast<std::uint32_t>(missing) * f.stride;
    StoreLocked(rseq, out);
    return static_cast<long>(rseq);
}

}  // namespace

int main() {
    const double t0       = EnvDouble("T0", 0.0);
    const double delay_ms = EnvDouble("DELAY_MS", 60.0);
    const double duration = EnvDouble("DURATION_S", 30.0);
    const std::uint32_t n_frames =
        static_cast<std::uint32_t>(duration * 1000.0 / 20.0);

    // Inbound: bind 47002.
    int in_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (in_fd < 0) { std::perror("socket in"); return 1; }
    {
        int one = 1;
        ::setsockopt(in_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        int rcvbuf = 1 << 21;
        ::setsockopt(in_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kRelayPort);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (::bind(in_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
            std::perror("bind 47002");
            return 1;
        }
    }

    // Outbound: player destination.
    int out_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (out_fd < 0) { std::perror("socket out"); return 1; }
    sockaddr_in player{};
    player.sin_family = AF_INET;
    player.sin_port = htons(kPlayerPort);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    std::atomic<bool> running{true};

    // ===================== RECEIVE THREAD =====================
    std::thread rx([&] {
        std::uint8_t buf[2048];
        while (running.load(std::memory_order_relaxed)) {
            ssize_t n = ::recvfrom(in_fd, buf, sizeof buf, 0, nullptr, nullptr);
            if (n <= 0) continue;

            if (buf[0] == kTypeData && n >= static_cast<ssize_t>(kDataPkt)) {
                std::uint32_t seq_be;
                std::memcpy(&seq_be, buf + 1, 4);
                const std::uint32_t seq = ntohl(seq_be);

                bool notify = false, recovered = false;
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    notify = StoreLocked(seq, buf + 5);
                    if (notify) {  // may complete a parity group awaiting it
                        for (FecRec& f : g_fec) {
                            if (f.used || f.k == 0) continue;
                            if (CoversLocked(f, seq) && TryRecoverLocked(f) >= 0)
                                recovered = true;
                        }
                    }
                }
                if (notify || recovered) g_cv.notify_all();

            } else if (buf[0] == kTypeFec && n >= static_cast<ssize_t>(kFecPkt)) {
                std::uint32_t base_be;
                std::memcpy(&base_be, buf + 1, 4);
                const std::uint32_t base = ntohl(base_be);
                const std::uint8_t k = buf[5];
                const std::uint8_t stride = buf[6];

                bool recovered = false;
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    FecRec& slot = g_fec[g_fec_head];
                    g_fec_head = (g_fec_head + 1) % kFecRing;
                    slot.base = base;
                    slot.k = k;
                    slot.stride = (stride == 0) ? 1 : stride;
                    slot.used = false;
                    std::memcpy(slot.xorp, buf + 7, kPayload);
                    recovered = TryRecoverLocked(slot) >= 0;
                }
                if (recovered) g_cv.notify_all();
            }
        }
    });

    // ===================== PLAYOUT THREAD =====================
    std::thread tx([&] {
        using Clock = std::chrono::system_clock;  // wall clock == harness clock
        std::uint8_t out_pkt[kHarnessPkt];

        for (std::uint32_t p = 0; p < n_frames; ++p) {
            const double deadline_s = t0 + delay_ms / 1000.0 + p * 0.020;
            const double send_by_s = deadline_s - kSendMarginS;
            const Clock::time_point send_by =
                Clock::time_point(std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(send_by_s)));

            std::uint8_t payload[kPayload];
            bool have = false;
            {
                std::unique_lock<std::mutex> lk(g_mu);
                g_cv.wait_until(lk, send_by, [&] { return IsPresentLocked(p); });
                if (IsPresentLocked(p)) {
                    std::memcpy(payload, g_buf[p & kMask].data, kPayload);
                    have = true;
                }
            }
            if (have) {
                std::uint32_t seq_be = htonl(p);
                std::memcpy(out_pkt, &seq_be, 4);
                std::memcpy(out_pkt + 4, payload, kPayload);
                ::sendto(out_fd, out_pkt, kHarnessPkt, 0,
                         reinterpret_cast<sockaddr*>(&player), sizeof player);
            }
        }
        running.store(false, std::memory_order_relaxed);
    });

    tx.join();
    running.store(false, std::memory_order_relaxed);
    ::shutdown(in_fd, SHUT_RDWR);
    ::close(in_fd);
    rx.join();
    ::close(out_fd);
    return 0;
}

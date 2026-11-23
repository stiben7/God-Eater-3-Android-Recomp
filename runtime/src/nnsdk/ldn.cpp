// nn::ldn (Local Play) over IP. The phone has no radio that speaks the Switch's protocol, so a network the game creates
// is a TCP service instead, reached two ways:
//  - through hub servers (the protocol of MHGU for Android's server/): a created network is listed
//    there for players with the same matchmaking key, the game's Scan shows the listed ones, and Connect joins one
//    through the server, which splices the player's connection to one the host opens for it. Nobody forwards a port.
//    Hubs are listed under this port's own link protocol (kLinkProtocol), so MH Rise only ever sees MH Rise hubs.
//  - directly: the host also listens on a TCP port (<datos>/hub_port, 0: off), and Scan asks the addresses in
//    <datos>/hub_hosts (one host[:port] per line) for their network.
// The game's packets go down the same connection (the host relays them between its players): socket.cpp hands every
// datagram for a 169.254.x.x address to ldn_net::send, and the ones that arrive are delivered to the game's socket by a
// loopback helper (ldn_net.h).
//
// Settings (the game folder, written by the app's Multiplayer settings): hub_servers (one host[:port] per line;
// "#" in front: kept in the list but not used; missing: the default server; "off": none), hub_key (the matchmaking key: only its SHA-256 is sent), hub_hosts,
// hub_port. Read when a network is created or searched for.
//
// Server control frames: u32 size (of what follows), u8 type, payload, little endian. The first one has a header:
// "MHGS", u16 server protocol (1), u16 link protocol, room[16] (SHA-256 of the key, the first 16 bytes; zeros without
// a key). LIST -> HUBS (u32 count, count x (u64 id, NetworkInfo)); HOST + NetworkInfo -> HOSTED (u64 id), then UPDATE
// / PING from the host and INCOMING (u64 ticket) / PING to it; ATTACH + ticket and JOIN + hub id -> SPLICED (u32 pair,
// u64 secret), after which the connection is a link.
//
// Link frames (this port's, over a spliced or direct connection): the same u32 size + u8 type framing.
//   HELLO   station -> host  u16 link version, u16 local communication version, u8 query only, mac[6], name[33]
//   WELCOME host -> station  u8 node id, u32 address, NetworkInfo
//   INFO    host -> station  NetworkInfo (players joined or left, advertise data changed; also a query's answer)
//   DATA    both ways        u32 from, u16 from port, u32 to, u16 to port, datagram
//   BYE     both ways        u32 reason
//   PING    both ways
//   REJECT  host -> station  u32 reason
//
// MakeIpv4Address is called by ~6000 of the game's static constructors, before nnMain and nnsdk_attach: it can't
// depend on anything.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "nnsdk/ldn_net.h"
#include "nnsdk/os.h"
#include "nnsdk/types.h"

namespace nn::ldn {

struct Ipv4Address {
    uint32_t raw;
};
struct SubnetMask {
    uint32_t raw;
};
struct NetworkInfo;
struct NetworkConfig;
struct ScanFilter;
struct SecurityConfig;
struct SecurityParameter;
struct UserConfig;
struct AddressEntry;

enum State {
    State_None = 0,
    State_Initialized = 1,
    State_AccessPoint = 2,
    State_AccessPointCreated = 3,
    State_Station = 4,
    State_StationConnected = 5,
    State_Error = 6,
};
enum DisconnectReason {
    DisconnectReason_None = 0,
    DisconnectReason_User = 1,
    DisconnectReason_DestroyedByUser = 3,
    DisconnectReason_SignalLost = 6,
};
enum AcceptPolicy {};
enum ConnectOption {};

namespace {

// ---- the SDK's structures, as bytes ----------------------------------------------------------------------------------

constexpr size_t kInfoSize = 0x480;
constexpr size_t kNiLocalId = 0x00, kNiSceneId = 0x0A, kNiSessionId = 0x10, kNiBssid = 0x20, kNiSsidLen = 0x26,
                 kNiSsid = 0x27, kNiChannel = 0x48, kNiLinkLevel = 0x4A, kNiNetType = 0x4B, kNiSecParam = 0x50,
                 kNiSecMode = 0x60, kNiAcceptPolicy = 0x62, kNiNodeMax = 0x66, kNiNodeCount = 0x67, kNiNodes = 0x68,
                 kNiAdvSize = 0x26A, kNiAdv = 0x26C;
constexpr size_t kNodeSize = 0x40, kNodeAddr = 0x00, kNodeMac = 0x04, kNodeId = 0x0A, kNodeConnected = 0x0B,
                 kNodeName = 0x0C, kNodeLcv = 0x2E;
constexpr size_t kCfgLocalId = 0x00, kCfgSceneId = 0x0A, kCfgChannel = 0x10, kCfgNodeMax = 0x12, kCfgLcv = 0x14,
                 kCfgSize = 0x20;
constexpr size_t kFilterSessionId = 0x10, kFilterNetType = 0x20, kFilterBssid = 0x24, kFilterSsidLen = 0x2A,
                 kFilterSsid = 0x2B, kFilterFlags = 0x5C;
constexpr uint32_t kFilterLocalId = 1, kFilterSession = 2, kFilterNetworkType = 4, kFilterMac = 8, kFilterSsidFlag = 16,
                   kFilterScene = 32;
constexpr size_t kNameSize = 33;
constexpr int kMaxNodes = 8;
constexpr size_t kAdvertiseDataSizeMax = 0x180;

constexpr Result kResultAdvertiseDataTooLarge{203 | (10 << 9)};
constexpr Result kResultConnectionFailed{203 | (31 << 9)};
constexpr Result kResultBadState{203 | (32 << 9)};

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) { return get16(p) | uint32_t{get16(p + 2)} << 16; }
uint64_t get64(const uint8_t* p) { return get32(p) | uint64_t{get32(p + 4)} << 32; }
void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v), p[1] = static_cast<uint8_t>(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, static_cast<uint16_t>(v)), put16(p + 2, static_cast<uint16_t>(v >> 16)); }
void put64(uint8_t* p, uint64_t v) { put32(p, static_cast<uint32_t>(v)), put32(p + 4, static_cast<uint32_t>(v >> 32)); }
void add16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(static_cast<uint8_t>(v)), b.push_back(static_cast<uint8_t>(v >> 8)); }
void add32(std::vector<uint8_t>& b, uint32_t v) { add16(b, static_cast<uint16_t>(v)), add16(b, static_cast<uint16_t>(v >> 16)); }
void add64(std::vector<uint8_t>& b, uint64_t v) { add32(b, static_cast<uint32_t>(v)), add32(b, static_cast<uint32_t>(v >> 32)); }

using Info = std::array<uint8_t, kInfoSize>;

// ---- settings ---------------------------------------------------------------------------------------------------------

constexpr uint16_t kServerProtocol = 1;
constexpr uint16_t kLinkProtocol = 0x5201;  // 'R', 1: MH Rise's hubs, apart from MHGU's (2, 3) and MH4U's (0x4D01)
constexpr uint16_t kLinkVersion = 1;
constexpr uint16_t kDefaultPort = 39510;
// (Not the MHGU hub servers: their owners asked for MH Rise hubs to stay off them.)
constexpr const char* kDefaultServers[] = {"meoli.viewdns.net"};
constexpr const char* kRetiredServer = "morerokk-mhgurecomp-ldnserver.duckdns.org";  // never used, even if listed

struct Endpoint {
    std::string host;
    uint16_t port = kDefaultPort;
};

std::string data_path(const char* name) {
    const char* data = getenv("NIER_DATA");
    return std::string(data ? data : ".") + "/" + name;
}

std::vector<std::string> read_lines(const char* name) {
    std::vector<std::string> out;
    std::ifstream f(data_path(name));
    for (std::string line; std::getline(f, line);) {
        const size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a != std::string::npos) out.push_back(line.substr(a, b - a + 1));
    }
    return out;
}

Endpoint parse_endpoint(std::string s) {
    for (const char* scheme : {"http://", "https://", "tcp://"})
        if (s.rfind(scheme, 0) == 0) s = s.substr(strlen(scheme));
    if (const size_t slash = s.find('/'); slash != std::string::npos) s.resize(slash);
    Endpoint e;
    if (const size_t colon = s.rfind(':'); colon != std::string::npos && s.find(':') == colon) {
        e.port = static_cast<uint16_t>(std::clamp(atoi(s.c_str() + colon + 1), 1, 65535));
        s.resize(colon);
    }
    e.host = s;
    return e;
}

std::vector<Endpoint> hub_servers() {
    std::vector<std::string> lines = read_lines("hub_servers");
    std::vector<Endpoint> out;
    if (lines.empty())
        for (const char* s : kDefaultServers) lines.push_back(s);
    for (const std::string& l : lines) {
        if (l == "off") return {};
        if (l.empty() || l[0] == '#') continue;
        Endpoint e = parse_endpoint(l);
        if (e.host == kRetiredServer) continue;  // (an older list saved with it)
        out.push_back(e);
    }
    return out;
}

std::vector<Endpoint> search_hosts() {
    std::vector<Endpoint> out;
    for (const std::string& l : read_lines("hub_hosts"))
        if (!l.empty() && l[0] != '#') out.push_back(parse_endpoint(l));
    return out;
}

uint16_t listen_port() {
    const std::vector<std::string> l = read_lines("hub_port");
    if (l.empty()) return kDefaultPort;
    return static_cast<uint16_t>(std::clamp(atoi(l[0].c_str()), 0, 65535));
}

// SHA-256, for the matchmaking key's room (FIPS 180-4)
void sha256(const uint8_t* data, size_t size, uint8_t out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> m(data, data + size);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back(static_cast<uint8_t>(uint64_t{size} * 8 >> (8 * i)));
    auto rotr = [](uint32_t x, int n) { return x >> n | x << (32 - n); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t{m[off + 4 * i]} << 24 | uint32_t{m[off + 4 * i + 1]} << 16 | uint32_t{m[off + 4 * i + 2]} << 8 |
                   m[off + 4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
}

std::array<uint8_t, 16> room_of_key() {
    std::array<uint8_t, 16> room{};
    const std::vector<std::string> l = read_lines("hub_key");
    if (l.empty() || l[0].empty()) return room;
    uint8_t digest[32];
    sha256(reinterpret_cast<const uint8_t*>(l[0].data()), l[0].size(), digest);
    memcpy(room.data(), digest, 16);
    return room;
}

uint64_t random64() {
    static std::mt19937_64 rng{std::random_device{}() ^ static_cast<uint64_t>(
                                   std::chrono::steady_clock::now().time_since_epoch().count())};
    static std::mutex m;
    std::lock_guard lock(m);
    return rng();
}

// ---- sockets ----------------------------------------------------------------------------------------------------------

constexpr int kConnectTimeoutMs = 4000;
constexpr int kAnswerTimeoutMs = 6000;
constexpr size_t kMaxServerFrame = 65600;
constexpr size_t kMaxLinkFrame = 16384;

int connect_to(const Endpoint& e, int timeout_ms) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port[8];
    snprintf(port, sizeof port, "%u", e.port);
    if (getaddrinfo(e.host.c_str(), port, &hints, &res) != 0 || !res) {
        printf("[ldn] %s: can't resolve the name\n", e.host.c_str());
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = res; a && fd < 0; a = a->ai_next) {
        fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) break;
        if (::connect(fd, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS) {
            ::close(fd), fd = -1;
            continue;
        }
        pollfd p{fd, POLLOUT, 0};
        int err = 0;
        socklen_t len = sizeof err;
        if (poll(&p, 1, timeout_ms) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err) {
            ::close(fd), fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    }
    freeaddrinfo(res);
    if (fd < 0) printf("[ldn] %s:%u: no connection\n", e.host.c_str(), e.port);
    return fd;
}

bool send_all(int fd, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size) {
        const ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n, size -= static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int fd, void* data, size_t size, int timeout_ms) {
    auto* p = static_cast<uint8_t*>(data);
    while (size) {
        if (timeout_ms >= 0) {
            pollfd pf{fd, POLLIN, 0};
            if (poll(&pf, 1, timeout_ms) != 1) return false;
        }
        const ssize_t n = ::recv(fd, p, size, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n, size -= static_cast<size_t>(n);
    }
    return true;
}

std::vector<uint8_t> make_frame(uint8_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f;
    add32(f, static_cast<uint32_t>(payload.size() + 1));
    f.push_back(type);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// One frame; timeout_ms < 0: wait as long as it takes. False on a closed connection, a timeout or a bad size.
bool read_frame(int fd, uint8_t* type, std::vector<uint8_t>* payload, size_t max, int timeout_ms) {
    uint8_t head[4];
    if (!recv_all(fd, head, 4, timeout_ms)) return false;
    const uint32_t size = get32(head);
    if (size < 1 || size > max + 1) return false;
    std::vector<uint8_t> body(size);
    if (!recv_all(fd, body.data(), size, timeout_ms < 0 ? -1 : std::max(timeout_ms, 3000))) return false;
    *type = body[0];
    payload->assign(body.begin() + 1, body.end());
    return true;
}

// ---- the hub server's protocol ---------------------------------------------------------------------------------------

enum : uint8_t { kList = 1, kHost = 2, kAttach = 3, kJoin = 4, kUpdate = 5, kPing = 6 };
enum : uint8_t { kHubs = 16, kHosted = 17, kIncoming = 18, kSpliced = 19, kError = 20 };

std::vector<uint8_t> server_header(const std::array<uint8_t, 16>& room) {
    std::vector<uint8_t> h = {'M', 'H', 'G', 'S'};
    add16(h, kServerProtocol);
    add16(h, kLinkProtocol);
    h.insert(h.end(), room.begin(), room.end());
    return h;
}

// ---- link frames ------------------------------------------------------------------------------------------------------

enum : uint8_t { kHello = 1, kWelcome = 2, kInfoFrame = 3, kData = 4, kBye = 5, kLinkPing = 6, kReject = 7 };
constexpr size_t kDataHeader = 12;

struct Link {
    int fd = -1;
    std::mutex send_mutex;
    int node = -1;           // host side: the station's node id (1..7)
    uint32_t address = 0;    // host side: its address
    std::atomic<bool> open{true};

    bool send(uint8_t type, const std::vector<uint8_t>& payload) {
        const std::vector<uint8_t> f = make_frame(type, payload);
        std::lock_guard lock(send_mutex);
        if (!open || !send_all(fd, f.data(), f.size())) {
            open = false;
            return false;
        }
        return true;
    }
    void close() {
        if (open.exchange(false)) ::shutdown(fd, SHUT_RDWR);
    }
};

// ---- the network ------------------------------------------------------------------------------------------------------

std::atomic<int> g_state{State_None};
std::atomic<os::SystemEventType*> g_state_event{nullptr};
std::atomic<int> g_disconnect_reason{DisconnectReason_None};

std::mutex g_mu;  // everything below
Info g_info{};    // the network this console is in (host: the one it made)
std::array<uint8_t, kCfgSize> g_config{};
std::array<uint8_t, 0x20> g_security{};  // SecurityParameter: data[16], session id[16]
uint32_t g_address = 0;                  // this console's address in it (host byte order)
uint8_t g_mac[6] = {};
std::vector<std::shared_ptr<Link>> g_links;  // host: its stations; station: the one link to the host
std::atomic<uint64_t> g_generation{0};       // a new network (or none): threads of an older one stop
std::vector<std::shared_ptr<Link>> g_server_conns;  // host: the HOST connections to the hub servers

// The last search's hubs: where each one was found
struct Found {
    Info info;
    int server = -1;  // index in hub_servers(); -1: a direct host
    uint64_t hub = 0;
    Endpoint direct;
};
std::vector<Found> g_found;

void signal_change() {
    if (os::SystemEventType* e = g_state_event.load()) nnsdk::signal_system_event(e);
}

void set_state(State s) {
    if (g_state.exchange(s) == s) return;
    signal_change();
}

bool initialized() { return g_state.load() != State_None; }

uint32_t node_address(const Info& info, int node) { return get32(info.data() + kNiNodes + node * kNodeSize + kNodeAddr); }

uint32_t subnet_of(const uint8_t* session) { return 0xA9FE0000u | uint32_t{1u + session[0] % 254u} << 8; }

// The helper that delivers other players' datagrams to the game's sockets (loopback)
int g_helper = -1;
std::atomic<uint16_t> g_helper_port{0};

void ensure_helper() {
    if (g_helper >= 0) return;
    g_helper = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in in{};
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (g_helper < 0 || ::bind(g_helper, reinterpret_cast<sockaddr*>(&in), sizeof in) != 0) {
        printf("[ldn] no loopback helper (%s): the game won't get other players' packets\n", strerror(errno));
        return;
    }
    socklen_t len = sizeof in;
    getsockname(g_helper, reinterpret_cast<sockaddr*>(&in), &len);
    g_helper_port = ntohs(in.sin_port);
}

void deliver(uint32_t from, uint16_t from_port, uint16_t to_port, const uint8_t* data, size_t size) {
    if (g_helper < 0 || size > 65000) return;
    std::vector<uint8_t> d(nnsdk::ldn_net::kDeliverHeader + size);
    memcpy(d.data(), nnsdk::ldn_net::kDeliverMagic, 4);
    const uint32_t from_n = htonl(from);
    const uint16_t port_n = htons(from_port);
    memcpy(d.data() + 4, &from_n, 4);
    memcpy(d.data() + 8, &port_n, 2);
    memcpy(d.data() + nnsdk::ldn_net::kDeliverHeader, data, size);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(nnsdk::ldn_net::real_port(to_port));
    ::sendto(g_helper, d.data(), d.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&to), sizeof to);
}

std::vector<uint8_t> data_frame(uint32_t from, uint16_t from_port, uint32_t to, uint16_t to_port, const void* p, size_t n) {
    std::vector<uint8_t> f;
    add32(f, from), add16(f, from_port), add32(f, to), add16(f, to_port);
    const auto* b = static_cast<const uint8_t*>(p);
    f.insert(f.end(), b, b + n);
    return f;
}

bool is_broadcast(uint32_t a) { return a == 0xFFFFFFFFu || (a & 0xFF) == 0xFF; }

// ---- host ------------------------------------------------------------------------------------------------------------

void update_servers() {
    std::vector<std::shared_ptr<Link>> conns;
    Info info;
    {
        std::lock_guard lock(g_mu);
        conns = g_server_conns;
        info = g_info;
    }
    const std::vector<uint8_t> body(info.begin(), info.end());
    for (auto& c : conns) c->send(kUpdate, body);
}

void broadcast_info() {
    std::vector<std::shared_ptr<Link>> links;
    std::vector<uint8_t> body;
    {
        std::lock_guard lock(g_mu);
        links = g_links;
        body.assign(g_info.begin(), g_info.end());
    }
    for (auto& l : links) l->send(kInfoFrame, body);
    update_servers();
    signal_change();
}

// A datagram that reached the host, from its own game (from == the host's address) or a station: delivered and passed on
void host_route(const std::shared_ptr<Link>& source, uint32_t from, uint16_t from_port, uint32_t to, uint16_t to_port,
                const uint8_t* data, size_t size) {
    const bool broadcast = is_broadcast(to);
    uint32_t self;
    std::vector<std::shared_ptr<Link>> links;
    {
        std::lock_guard lock(g_mu);
        self = g_address;
        links = g_links;
    }
    if ((broadcast || to == self) && source) deliver(from, from_port, to_port, data, size);
    if (!broadcast && to == self) return;
    const std::vector<uint8_t> f = data_frame(from, from_port, to, to_port, data, size);
    for (auto& l : links)
        if (l != source && (broadcast || l->address == to)) l->send(kData, f);
}

void host_drop(const std::shared_ptr<Link>& link) {
    link->close();
    bool changed = false;
    {
        std::lock_guard lock(g_mu);
        auto it = std::find(g_links.begin(), g_links.end(), link);
        if (it != g_links.end()) {
            g_links.erase(it);
            if (link->node > 0) {
                uint8_t* n = g_info.data() + kNiNodes + link->node * kNodeSize;
                memset(n, 0, kNodeSize);
                g_info[kNiNodeCount] = static_cast<uint8_t>(std::max(1, g_info[kNiNodeCount] - 1));
                changed = true;
            }
        }
    }
    ::close(link->fd);
    if (changed) {
        printf("[ldn] player %d left\n", link->node);
        broadcast_info();
    }
}

// A station's connection to the host (direct or spliced by a server): its hello, a place in the network, then its packets
void host_serve(int fd, uint64_t generation) {
    auto link = std::make_shared<Link>();
    link->fd = fd;
    uint8_t type = 0;
    std::vector<uint8_t> p;
    if (!read_frame(fd, &type, &p, kMaxLinkFrame, kAnswerTimeoutMs) || type != kHello || p.size() < 11 + kNameSize ||
        get16(p.data()) != kLinkVersion) {
        if (type == kHello) link->send(kReject, {4, 0, 0, 0});
        ::close(fd);
        return;
    }
    const bool query = p[4] != 0;
    if (query) {  // a search: the network, then goodbye
        Info info;
        {
            std::lock_guard lock(g_mu);
            info = g_info;
        }
        link->send(kInfoFrame, std::vector<uint8_t>(info.begin(), info.end()));
        ::close(fd);
        return;
    }
    {
        std::lock_guard lock(g_mu);
        if (g_generation != generation || g_state != State_AccessPointCreated) {
            ::close(fd);
            return;
        }
        const int max = std::min<int>(g_info[kNiNodeMax], kMaxNodes);
        for (int n = 1; n < max; n++)
            if (!g_info[kNiNodes + n * kNodeSize + kNodeConnected]) {
                link->node = n;
                break;
            }
        if (link->node > 0) {
            uint8_t* node = g_info.data() + kNiNodes + link->node * kNodeSize;
            memset(node, 0, kNodeSize);
            link->address = (g_address & 0xFFFFFF00u) | static_cast<uint32_t>(link->node + 1);
            put32(node + kNodeAddr, link->address);
            memcpy(node + kNodeMac, p.data() + 5, 6);
            node[kNodeId] = static_cast<uint8_t>(link->node);
            node[kNodeConnected] = 1;
            memcpy(node + kNodeName, p.data() + 11, kNameSize - 1);
            put16(node + kNodeLcv, get16(p.data() + 2));
            g_info[kNiNodeCount] = static_cast<uint8_t>(g_info[kNiNodeCount] + 1);
            g_links.push_back(link);
        }
    }
    if (link->node < 0) {
        link->send(kReject, {3, 0, 0, 0});
        ::close(fd);
        return;
    }
    std::vector<uint8_t> welcome;
    welcome.push_back(static_cast<uint8_t>(link->node));
    add32(welcome, link->address);
    {
        std::lock_guard lock(g_mu);
        welcome.insert(welcome.end(), g_info.begin(), g_info.end());
    }
    link->send(kWelcome, welcome);
    printf("[ldn] player %d joined (%u.%u.%u.%u)\n", link->node, link->address >> 24, link->address >> 16 & 255,
           link->address >> 8 & 255, link->address & 255);
    broadcast_info();
    while (g_generation == generation && link->open) {
        if (!read_frame(fd, &type, &p, kMaxLinkFrame, 30000)) break;
        if (type == kData && p.size() >= kDataHeader)
            host_route(link, link->address, get16(p.data() + 4), get32(p.data() + 6), get16(p.data() + 10),
                       p.data() + kDataHeader, p.size() - kDataHeader);
        else if (type == kBye)
            break;
    }
    host_drop(link);
}

// The host's listing on one hub server: HOST, then tickets for players who join (ATTACH opens their link)
void host_on_server(Endpoint server, uint64_t generation) {
    const std::array<uint8_t, 16> room = room_of_key();
    while (g_generation == generation) {
        const int fd = connect_to(server, kConnectTimeoutMs);
        if (fd >= 0) {
            auto conn = std::make_shared<Link>();
            conn->fd = fd;
            std::vector<uint8_t> body = server_header(room);
            {
                std::lock_guard lock(g_mu);
                body.insert(body.end(), g_info.begin(), g_info.end());
            }
            uint8_t type;
            std::vector<uint8_t> p;
            if (conn->send(kHost, body) && read_frame(fd, &type, &p, kMaxServerFrame, kAnswerTimeoutMs) &&
                type == kHosted && p.size() >= 8) {
                printf("[ldn] hub listed on %s (id %016llx)\n", server.host.c_str(),
                       static_cast<unsigned long long>(get64(p.data())));
                {
                    std::lock_guard lock(g_mu);
                    g_server_conns.push_back(conn);
                }
                update_servers();  // (players may have joined meanwhile)
                auto last_ping = std::chrono::steady_clock::now();
                while (g_generation == generation && conn->open) {
                    if (std::chrono::steady_clock::now() - last_ping > std::chrono::seconds(15)) {
                        conn->send(kPing, {});
                        last_ping = std::chrono::steady_clock::now();
                    }
                    pollfd pf{fd, POLLIN, 0};
                    if (poll(&pf, 1, 1000) == 0) continue;
                    if (!read_frame(fd, &type, &p, kMaxServerFrame, 5000)) break;
                    if (type == kIncoming && p.size() >= 8) {
                        const uint64_t ticket = get64(p.data());
                        std::thread([server, room, ticket, generation] {
                            const int afd = connect_to(server, kConnectTimeoutMs);
                            if (afd < 0) return;
                            std::vector<uint8_t> req = server_header(room);
                            add64(req, ticket);
                            const std::vector<uint8_t> f = make_frame(kAttach, req);
                            uint8_t t;
                            std::vector<uint8_t> a;
                            if (!send_all(afd, f.data(), f.size()) || !read_frame(afd, &t, &a, kMaxServerFrame, kAnswerTimeoutMs) ||
                                t != kSpliced) {
                                ::close(afd);
                                return;
                            }
                            host_serve(afd, generation);
                        }).detach();
                    }
                }
                std::lock_guard lock(g_mu);
                g_server_conns.erase(std::remove(g_server_conns.begin(), g_server_conns.end(), conn), g_server_conns.end());
            } else if (type == kError && p.size() >= 4) {
                printf("[ldn] %s didn't list the hub (error %u)\n", server.host.c_str(), get32(p.data()));
            }
            conn->close();
            ::close(fd);
        }
        for (int i = 0; i < 100 && g_generation == generation; i++)  // 10 s, then again
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int g_listen = -1;

void host_listen(uint64_t generation) {
    while (g_generation == generation) {
        pollfd pf{g_listen, POLLIN, 0};
        if (poll(&pf, 1, 500) != 1) continue;
        const int fd = ::accept4(g_listen, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        std::thread(host_serve, fd, generation).detach();
    }
}

// ---- station ---------------------------------------------------------------------------------------------------------

void station_run(std::shared_ptr<Link> link, uint64_t generation) {
    uint8_t type;
    std::vector<uint8_t> p;
    int reason = DisconnectReason_SignalLost;
    auto last_ping = std::chrono::steady_clock::now();
    while (g_generation == generation && link->open) {
        if (std::chrono::steady_clock::now() - last_ping > std::chrono::seconds(10)) {
            link->send(kLinkPing, {});
            last_ping = std::chrono::steady_clock::now();
        }
        pollfd pf{link->fd, POLLIN, 0};
        if (poll(&pf, 1, 1000) == 0) continue;
        if (!read_frame(link->fd, &type, &p, kMaxLinkFrame, 30000)) break;
        if (type == kData && p.size() >= kDataHeader) {
            deliver(get32(p.data()), get16(p.data() + 4), get16(p.data() + 10), p.data() + kDataHeader,
                    p.size() - kDataHeader);
        } else if (type == kInfoFrame && p.size() == kInfoSize) {
            {
                std::lock_guard lock(g_mu);
                memcpy(g_info.data(), p.data(), kInfoSize);
            }
            signal_change();
        } else if (type == kBye) {
            reason = DisconnectReason_DestroyedByUser;
            break;
        }
    }
    link->close();
    ::close(link->fd);
    std::lock_guard lock(g_mu);
    if (g_generation != generation) return;
    g_links.clear();
    g_address = 0;
    g_disconnect_reason = reason;
    printf("[ldn] disconnected from the host\n");
    g_state = State_Station;
    signal_change();
}

// Hello, welcome: the station's place in the network
bool station_join(int fd, const UserConfig& user, uint16_t lcv) {
    std::vector<uint8_t> hello;
    add16(hello, kLinkVersion);
    add16(hello, lcv);
    hello.push_back(0);
    hello.insert(hello.end(), g_mac, g_mac + 6);
    const auto* name = reinterpret_cast<const uint8_t*>(&user);
    hello.insert(hello.end(), name, name + kNameSize);
    const std::vector<uint8_t> f = make_frame(kHello, hello);
    uint8_t type;
    std::vector<uint8_t> p;
    if (!send_all(fd, f.data(), f.size()) || !read_frame(fd, &type, &p, kMaxLinkFrame, kAnswerTimeoutMs)) return false;
    if (type == kReject) {
        printf("[ldn] the host turned us away (%u)\n", p.size() >= 4 ? get32(p.data()) : 0);
        return false;
    }
    if (type != kWelcome || p.size() != 5 + kInfoSize) return false;
    const uint64_t generation = ++g_generation;
    auto link = std::make_shared<Link>();
    link->fd = fd;
    {
        std::lock_guard lock(g_mu);
        g_address = get32(p.data() + 1);
        memcpy(g_info.data(), p.data() + 5, kInfoSize);
        g_links = {link};
        g_disconnect_reason = DisconnectReason_None;
    }
    ensure_helper();
    printf("[ldn] in the network as player %u (%u.%u.%u.%u)\n", p[0], g_address >> 24, g_address >> 16 & 255,
           g_address >> 8 & 255, g_address & 255);
    std::thread(station_run, link, generation).detach();
    return true;
}

// ---- search ----------------------------------------------------------------------------------------------------------

std::vector<Found> list_server(int index, const Endpoint& server, const std::array<uint8_t, 16>& room) {
    std::vector<Found> out;
    const int fd = connect_to(server, kConnectTimeoutMs);
    if (fd < 0) return out;
    const std::vector<uint8_t> f = make_frame(kList, server_header(room));
    uint8_t type;
    std::vector<uint8_t> p;
    if (send_all(fd, f.data(), f.size()) && read_frame(fd, &type, &p, kMaxServerFrame, kAnswerTimeoutMs) && type == kHubs &&
        p.size() >= 4) {
        const uint32_t count = get32(p.data());
        for (uint32_t i = 0; i < count && 4 + (i + 1) * (8 + kInfoSize) <= p.size(); i++) {
            Found h;
            h.server = index;
            h.hub = get64(p.data() + 4 + i * (8 + kInfoSize));
            memcpy(h.info.data(), p.data() + 4 + i * (8 + kInfoSize) + 8, kInfoSize);
            out.push_back(h);
        }
    }
    ::close(fd);
    return out;
}

std::vector<Found> query_host(const Endpoint& host) {
    std::vector<Found> out;
    const int fd = connect_to(host, 2500);
    if (fd < 0) return out;
    std::vector<uint8_t> hello;
    add16(hello, kLinkVersion);
    add16(hello, 0);
    hello.push_back(1);
    hello.insert(hello.end(), 6 + kNameSize, 0);
    const std::vector<uint8_t> f = make_frame(kHello, hello);
    uint8_t type;
    std::vector<uint8_t> p;
    if (send_all(fd, f.data(), f.size()) && read_frame(fd, &type, &p, kMaxLinkFrame, 3000) && type == kInfoFrame &&
        p.size() == kInfoSize) {
        Found h;
        memcpy(h.info.data(), p.data(), kInfoSize);
        h.direct = host;
        out.push_back(h);
    }
    ::close(fd);
    return out;
}

bool matches(const Info& info, const ScanFilter& filter_ref) {
    const auto* f = reinterpret_cast<const uint8_t*>(&filter_ref);
    const uint32_t flags = get32(f + kFilterFlags);
    if ((flags & kFilterLocalId) && get64(f) != get64(info.data() + kNiLocalId)) return false;
    if ((flags & kFilterScene) && get16(f + kNiSceneId) != get16(info.data() + kNiSceneId)) return false;
    if ((flags & kFilterSession) && memcmp(f + kFilterSessionId, info.data() + kNiSessionId, 16) != 0) return false;
    if ((flags & kFilterNetworkType) && (get32(f + kFilterNetType) & info[kNiNetType]) == 0) return false;
    if ((flags & kFilterMac) && memcmp(f + kFilterBssid, info.data() + kNiBssid, 6) != 0) return false;
    if ((flags & kFilterSsidFlag) &&
        (f[kFilterSsidLen] != info[kNiSsidLen] || memcmp(f + kFilterSsid, info.data() + kNiSsid, f[kFilterSsidLen]) != 0))
        return false;
    return true;
}

Result scan(NetworkInfo* out, int* count, int max, const ScanFilter& filter) {
    if (!initialized()) return kResultBadState;
    const std::vector<Endpoint> servers = hub_servers();
    const std::vector<Endpoint> hosts = search_hosts();
    const std::array<uint8_t, 16> room = room_of_key();
    std::vector<std::vector<Found>> results(servers.size() + hosts.size());
    std::vector<std::thread> threads;
    for (size_t i = 0; i < servers.size(); i++)
        threads.emplace_back([&, i] { results[i] = list_server(static_cast<int>(i), servers[i], room); });
    for (size_t i = 0; i < hosts.size(); i++)
        threads.emplace_back([&, i] { results[servers.size() + i] = query_host(hosts[i]); });
    for (auto& t : threads) t.join();
    std::vector<Found> all;
    for (auto& r : results)
        for (Found& h : r) {
            if (!matches(h.info, filter)) continue;
            // (the same hub on two servers: once)
            const bool dup = std::any_of(all.begin(), all.end(), [&](const Found& o) {
                return memcmp(o.info.data() + kNiSessionId, h.info.data() + kNiSessionId, 16) == 0;
            });
            if (!dup) all.push_back(h);
        }
    int n = 0;
    for (const Found& h : all) {
        if (n >= max) break;
        memcpy(reinterpret_cast<uint8_t*>(out) + n * kInfoSize, h.info.data(), kInfoSize);
        n++;
    }
    *count = n;
    {
        std::lock_guard lock(g_mu);
        g_found = std::move(all);
    }
    static int logged = 0;
    if (logged++ < 20) printf("[ldn] search: %d hubs (%zu servers, %zu direct hosts)\n", n, servers.size(), hosts.size());
    return ResultSuccess;
}

Result connect(const uint8_t* session, const UserConfig& user, uint16_t lcv) {
    if (g_state.load() != State_Station) return kResultBadState;
    Found target;
    bool known = false;
    {
        std::lock_guard lock(g_mu);
        for (const Found& h : g_found)
            if (memcmp(h.info.data() + kNiSessionId, session, 16) == 0) target = h, known = true;
    }
    if (!known) {
        printf("[ldn] join: that hub isn't in the last search\n");
        return kResultConnectionFailed;
    }
    int fd = -1;
    if (target.server >= 0) {
        const std::vector<Endpoint> servers = hub_servers();
        if (target.server >= static_cast<int>(servers.size())) return kResultConnectionFailed;
        fd = connect_to(servers[target.server], kConnectTimeoutMs);
        if (fd < 0) return kResultConnectionFailed;
        std::vector<uint8_t> req = server_header(room_of_key());
        add64(req, target.hub);
        const std::vector<uint8_t> f = make_frame(kJoin, req);
        uint8_t type;
        std::vector<uint8_t> p;
        if (!send_all(fd, f.data(), f.size()) || !read_frame(fd, &type, &p, kMaxServerFrame, 15000) || type != kSpliced) {
            printf("[ldn] join: the server didn't connect us (%s)\n",
                   type == kError && p.size() >= 4 ? (get32(p.data()) == 2 ? "the host didn't answer" : "error") : "no answer");
            ::close(fd);
            return kResultConnectionFailed;
        }
    } else {
        fd = connect_to(target.direct, kConnectTimeoutMs);
        if (fd < 0) return kResultConnectionFailed;
    }
    if (!station_join(fd, user, lcv)) {
        ::close(fd);
        return kResultConnectionFailed;
    }
    set_state(State_StationConnected);
    return ResultSuccess;
}

// Leaves the network (host or station): threads of this one stop, links close
void leave(bool tell) {
    std::vector<std::shared_ptr<Link>> links, conns;
    {
        std::lock_guard lock(g_mu);
        ++g_generation;
        links.swap(g_links);
        conns.swap(g_server_conns);
        g_address = 0;
    }
    for (auto& l : links) {
        if (tell) l->send(kBye, {3, 0, 0, 0});
        l->close();
    }
    for (auto& c : conns) c->close();
    if (g_listen >= 0) ::close(g_listen), g_listen = -1;
}

Result create(const NetworkConfig& config_ref, const SecurityConfig& security_ref, const uint8_t* parameter,
              const UserConfig& user) {
    if (g_state.load() != State_AccessPoint) return kResultBadState;
    const auto* cfg = reinterpret_cast<const uint8_t*>(&config_ref);
    const auto* sec = reinterpret_cast<const uint8_t*>(&security_ref);
    leave(false);
    uint64_t generation;
    {
        std::lock_guard lock(g_mu);
        generation = g_generation;
        memcpy(g_config.data(), cfg, kCfgSize);
        // (advertise data the game set before creating the network stays)
        uint8_t adv[2 + kAdvertiseDataSizeMax];
        memcpy(adv, g_info.data() + kNiAdvSize, sizeof adv);
        g_info.fill(0);
        memcpy(g_info.data() + kNiAdvSize, adv, sizeof adv);
        uint8_t* i = g_info.data();
        put64(i + kNiLocalId, get64(cfg + kCfgLocalId));
        put16(i + kNiSceneId, get16(cfg + kCfgSceneId));
        if (parameter) {
            memcpy(i + kNiSecParam, parameter, 16);
            memcpy(i + kNiSessionId, parameter + 16, 16);
        } else {
            for (int k = 0; k < 2; k++) put64(i + kNiSessionId + 8 * k, random64()), put64(i + kNiSecParam + 8 * k, random64());
        }
        memcpy(g_security.data(), i + kNiSecParam, 16);
        memcpy(g_security.data() + 16, i + kNiSessionId, 16);
        memcpy(i + kNiBssid, g_mac, 6);
        static const char hex[] = "0123456789abcdef";
        i[kNiSsidLen] = 32;
        for (int k = 0; k < 32; k++) i[kNiSsid + k] = static_cast<uint8_t>(hex[(i[kNiSessionId + k / 2] >> (k % 2 ? 0 : 4)) & 15]);
        put16(i + kNiChannel, get16(cfg + kCfgChannel) ? get16(cfg + kCfgChannel) : 6);
        i[kNiLinkLevel] = 3;
        i[kNiNetType] = 2;
        put16(i + kNiSecMode, get16(sec));
        i[kNiNodeMax] = static_cast<uint8_t>(std::clamp<int>(cfg[kCfgNodeMax], 1, kMaxNodes));
        i[kNiNodeCount] = 1;
        g_address = subnet_of(i + kNiSessionId) | 1;
        uint8_t* host = i + kNiNodes;
        put32(host + kNodeAddr, g_address);
        memcpy(host + kNodeMac, g_mac, 6);
        host[kNodeId] = 0;
        host[kNodeConnected] = 1;
        memcpy(host + kNodeName, &user, kNameSize - 1);
        put16(host + kNodeLcv, get16(cfg + kCfgLcv));
    }
    ensure_helper();
    const uint16_t port = listen_port();
    if (port) {
        g_listen = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const int one = 1;
        setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_port = htons(port);
        if (g_listen >= 0 && ::bind(g_listen, reinterpret_cast<sockaddr*>(&in), sizeof in) == 0 && ::listen(g_listen, 8) == 0) {
            std::thread(host_listen, generation).detach();
        } else {
            printf("[ldn] can't listen on port %u (%s): direct joins off\n", port, strerror(errno));
            if (g_listen >= 0) ::close(g_listen), g_listen = -1;
        }
    }
    const std::vector<Endpoint> servers = hub_servers();
    for (const Endpoint& s : servers) std::thread(host_on_server, s, generation).detach();
    printf("[ldn] network created (%u players at most; %zu hub servers, port %u)\n", g_info[kNiNodeMax], servers.size(), port);
    set_state(State_AccessPointCreated);
    return ResultSuccess;
}

void init_mac() {
    if (g_mac[0]) return;
    const uint64_t r = random64();
    g_mac[0] = 0x02;  // locally administered
    for (int k = 1; k < 6; k++) g_mac[k] = static_cast<uint8_t>(r >> (8 * k));
}

}  // namespace

Ipv4Address MakeIpv4Address(unsigned char a, unsigned char b, unsigned char c, unsigned char d) {
    return {uint32_t{a} << 24 | uint32_t{b} << 16 | uint32_t{c} << 8 | d};
}

Ipv4Address MakeBroadcastAddress(Ipv4Address address, SubnetMask mask) { return {address.raw | ~mask.raw}; }

Result Initialize() {
    init_mac();
    if (!initialized()) set_state(State_Initialized);
    return ResultSuccess;
}

void Finalize() {
    leave(true);
    g_state = State_None;
    g_state_event = nullptr;  // (the game destroys it afterwards)
}

State GetState() { return static_cast<State>(g_state.load()); }

void AttachStateChangeEvent(os::SystemEventType* event) { g_state_event = event; }

Result OpenAccessPoint() {
    if (!initialized()) return kResultBadState;
    set_state(State_AccessPoint);
    return ResultSuccess;
}

Result CloseAccessPoint() {
    const int s = g_state.load();
    if (s == State_AccessPoint || s == State_AccessPointCreated) {
        leave(true);
        set_state(State_Initialized);
    }
    return ResultSuccess;
}

Result OpenStation() {
    if (!initialized()) return kResultBadState;
    set_state(State_Station);
    return ResultSuccess;
}

Result CloseStation() {
    const int s = g_state.load();
    if (s == State_Station || s == State_StationConnected) {
        leave(true);
        set_state(State_Initialized);
    }
    return ResultSuccess;
}

Result CreateNetwork(const NetworkConfig& config, const SecurityConfig& security, const UserConfig& user) {
    return create(config, security, nullptr, user);
}

Result CreateNetworkPrivate(const NetworkConfig& config, const SecurityConfig& security, const SecurityParameter& parameter,
                            const UserConfig& user, int, const AddressEntry*) {
    return create(config, security, reinterpret_cast<const uint8_t*>(&parameter), user);
}

Result DestroyNetwork() {
    if (g_state.load() != State_AccessPointCreated) return ResultSuccess;
    leave(true);
    set_state(State_AccessPoint);
    return ResultSuccess;
}

Result SetAdvertiseData(const void* data, size_t size) {
    if (size > kAdvertiseDataSizeMax) return kResultAdvertiseDataTooLarge;
    {
        std::lock_guard lock(g_mu);
        put16(g_info.data() + kNiAdvSize, static_cast<uint16_t>(size));
        memset(g_info.data() + kNiAdv, 0, kAdvertiseDataSizeMax);
        if (size) memcpy(g_info.data() + kNiAdv, data, size);
    }
    if (g_state.load() == State_AccessPointCreated) broadcast_info();
    return ResultSuccess;
}

Result SetStationAcceptPolicy(AcceptPolicy policy) {
    std::lock_guard lock(g_mu);
    g_info[kNiAcceptPolicy] = static_cast<uint8_t>(policy);
    return ResultSuccess;
}

Result Reject(Ipv4Address address) {
    std::shared_ptr<Link> link;
    {
        std::lock_guard lock(g_mu);
        for (auto& l : g_links)
            if (l->address == address.raw) link = l;
    }
    if (link) {
        link->send(kBye, {1, 0, 0, 0});
        link->close();
    }
    return ResultSuccess;
}

Result Scan(NetworkInfo* out, int* count, int max, const ScanFilter& filter, int) { return scan(out, count, max, filter); }

Result ScanPrivate(NetworkInfo* out, int* count, int max, const ScanFilter& filter, int) {
    return scan(out, count, max, filter);
}

Result Connect(const NetworkInfo& info, const SecurityConfig&, const UserConfig& user, int lcv, ConnectOption) {
    return connect(reinterpret_cast<const uint8_t*>(&info) + kNiSessionId, user, static_cast<uint16_t>(lcv));
}

Result ConnectPrivate(const NetworkConfig&, const SecurityConfig&, const SecurityParameter& parameter,
                      const UserConfig& user, int lcv, ConnectOption) {
    return connect(reinterpret_cast<const uint8_t*>(&parameter) + 16, user, static_cast<uint16_t>(lcv));
}

Result Disconnect() {
    if (g_state.load() == State_StationConnected) {
        leave(true);
        g_disconnect_reason = DisconnectReason_User;
        set_state(State_Station);
    }
    return ResultSuccess;
}

DisconnectReason GetDisconnectReason() { return static_cast<DisconnectReason>(g_disconnect_reason.load()); }

Result GetIpv4Address(Ipv4Address* address, SubnetMask* mask) {
    std::lock_guard lock(g_mu);
    const int s = g_state.load();
    if ((s != State_AccessPointCreated && s != State_StationConnected) || !g_address) {
        address->raw = mask->raw = 0;
        return kResultBadState;
    }
    address->raw = g_address;
    mask->raw = 0xFFFFFF00u;
    return ResultSuccess;
}

Result GetNetworkInfo(NetworkInfo* out) {
    const int s = g_state.load();
    if (s != State_AccessPointCreated && s != State_StationConnected) return kResultBadState;
    std::lock_guard lock(g_mu);
    memcpy(out, g_info.data(), kInfoSize);
    return ResultSuccess;
}

Result GetNetworkConfig(NetworkConfig* out) {
    const int s = g_state.load();
    if (s != State_AccessPointCreated && s != State_StationConnected) return kResultBadState;
    std::lock_guard lock(g_mu);
    if (s == State_StationConnected) {  // (a station has no config of its own: the host's network's)
        uint8_t c[kCfgSize] = {};
        put64(c + kCfgLocalId, get64(g_info.data() + kNiLocalId));
        put16(c + kCfgSceneId, get16(g_info.data() + kNiSceneId));
        put16(c + kCfgChannel, get16(g_info.data() + kNiChannel));
        c[kCfgNodeMax] = g_info[kNiNodeMax];
        put16(c + kCfgLcv, get16(g_info.data() + kNiNodes + kNodeLcv));
        memcpy(out, c, kCfgSize);
    } else {
        memcpy(out, g_config.data(), kCfgSize);
    }
    return ResultSuccess;
}

Result GetSecurityParameter(SecurityParameter* out) {
    const int s = g_state.load();
    if (s != State_AccessPointCreated && s != State_StationConnected) return kResultBadState;
    std::lock_guard lock(g_mu);
    uint8_t p[0x20];
    memcpy(p, g_info.data() + kNiSecParam, 16);
    memcpy(p + 16, g_info.data() + kNiSessionId, 16);
    memcpy(out, p, sizeof p);
    return ResultSuccess;
}

}  // namespace nn::ldn

namespace nnsdk::ldn_net {

using namespace nn::ldn;

bool active() {
    const int s = g_state.load();
    return (s == State_AccessPointCreated || s == State_StationConnected) && g_address != 0;
}

uint32_t own_address() { return active() ? g_address : 0; }

bool in_network(uint32_t address) {
    if (!active()) return false;
    return address == 0xFFFFFFFFu || (address & 0xFFFF0000u) == 0xA9FE0000u;
}

uint16_t helper_port() { return g_helper_port.load(); }

namespace {
std::mutex g_ports_m;
std::vector<std::pair<uint16_t, uint16_t>> g_ports;  // (game, real)
}  // namespace

void map_port(uint16_t game, uint16_t real) {
    std::lock_guard lock(g_ports_m);
    g_ports.erase(std::remove_if(g_ports.begin(), g_ports.end(), [&](const auto& p) { return p.first == game; }),
                  g_ports.end());
    if (real && real != game) g_ports.emplace_back(game, real);
}

uint16_t real_port(uint16_t game) {
    std::lock_guard lock(g_ports_m);
    for (const auto& p : g_ports)
        if (p.first == game) return p.second;
    return game;
}

uint16_t game_port(uint16_t real) {
    std::lock_guard lock(g_ports_m);
    for (const auto& p : g_ports)
        if (p.second == real) return p.first;
    return real;
}

void send(uint16_t src_port, uint32_t dst, uint16_t dst_port, const void* data, size_t size) {
    if (!active() || size > kMaxLinkFrame - 64) return;
    const uint32_t self = g_address;
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (dst == self) {  // to itself
        deliver(self, src_port, dst_port, bytes, size);
        return;
    }
    if (g_state.load() == State_AccessPointCreated) {
        host_route(nullptr, self, src_port, dst, dst_port, bytes, size);
        return;
    }
    std::shared_ptr<Link> link;
    {
        std::lock_guard lock(g_mu);
        if (!g_links.empty()) link = g_links[0];
    }
    if (link) link->send(kData, data_frame(self, src_port, dst, dst_port, data, size));
}

}  // namespace nnsdk::ldn_net

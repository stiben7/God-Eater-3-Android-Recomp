// The local wireless network that ldn.cpp makes over the internet (hub servers) or a direct connection, as seen from
// nn::socket (socket.cpp). While the game is in a network, UDP it sends to 169.254.x.x (the network's addresses) goes
// to ldn.cpp instead of the phone's network, and what other players send arrives on the game's socket from a
// loopback helper, with a small header in front that names the real sender (stripped again in socket.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace nnsdk::ldn_net {

// "LDV1", u32 sender's address, u16 sender's port (both network byte order), then the datagram
constexpr uint8_t kDeliverMagic[4] = {'L', 'D', 'V', '1'};
constexpr size_t kDeliverHeader = 10;

// In a network right now (hosting one that was created, or connected to one)
bool active();
// This console's address in the network (host byte order), 0 outside one
uint32_t own_address();
// An address that belongs to the network (its subnet, or 255.255.255.255), host byte order
bool in_network(uint32_t address);
// The loopback port deliveries come from (0: none yet)
uint16_t helper_port();
// A datagram from one of the game's sockets (its local port) to an address in the network
void send(uint16_t src_port, uint32_t dst_address, uint16_t dst_port, const void* data, size_t size);

// Two copies of the game on one phone (a second install under another package name, its own data folder) both bind
// the game's UDP port; the second one gets another real port (socket.cpp), and these tell the two apart. Ports the
// game sees ("game") and the ones it really has ("real"); with no mapping they're the same.
void map_port(uint16_t game, uint16_t real);  // real 0: drop the mapping
uint16_t real_port(uint16_t game);
uint16_t game_port(uint16_t real);

}  // namespace nnsdk::ldn_net

/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if defined(WEBRTC_POSIX)
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#ifdef OPENBSD
#include <netinet/in_systm.h>
#endif
#ifndef __native_client__
#include <netinet/ip.h>
#endif
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

#include <stdio.h>

#include "webrtc/base/ipaddress.h"
#include "webrtc/base/byteorder.h"
#include "webrtc/base/nethelpers.h"
#include "webrtc/base/logging.h"
#include "webrtc/base/win32.h"

namespace rtc {

// Prefixes used for categorizing IPv6 addresses.
static const in6_addr kV4MappedPrefix = {{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                           0xFF, 0xFF, 0}}};
static const in6_addr k6To4Prefix = {{{0x20, 0x02, 0}}};
static const in6_addr kTeredoPrefix = {{{0x20, 0x01, 0x00, 0x00}}};
static const in6_addr kV4CompatibilityPrefix = {{{0}}};
static const in6_addr k6BonePrefix = {{{0x3f, 0xfe, 0}}};

static bool IsPrivateV4(uint32_t ip);
static in_addr ExtractMappedAddress(const in6_addr& addr);

uint32_t IPAddress::v4AddressAsHostOrderInteger() const {
  if (family_ == AF_INET) {
    return NetworkToHost32(u_.ip4.s_addr);
  } else {
    return 0;
  }
}

bool IPAddress::IsNil() const {
  return IPIsUnspec(*this);
}

size_t IPAddress::Size() const {
  switch (family_) {
    case AF_INET:
      return sizeof(in_addr);
    case AF_INET6:
      return sizeof(in6_addr);
  }
  return 0;
}


bool IPAddress::operator==(const IPAddress &other) const {
  if (family_ != other.family_) {
    return false;
  }
  if (family_ == AF_INET) {
    return memcmp(&u_.ip4, &other.u_.ip4, sizeof(u_.ip4)) == 0;
  }
  if (family_ == AF_INET6) {
    return memcmp(&u_.ip6, &other.u_.ip6, sizeof(u_.ip6)) == 0;
  }
  return family_ == AF_UNSPEC;
}

bool IPAddress::operator!=(const IPAddress &other) const {
  return !((*this) == other);
}

bool IPAddress::operator >(const IPAddress &other) const {
  return (*this) != other && !((*this) < other);
}

bool IPAddress::operator <(const IPAddress &other) const {
  // IPv4 is 'less than' IPv6
  if (family_ != other.family_) {
    if (family_ == AF_UNSPEC) {
      return true;
    }
    if (family_ == AF_INET && other.family_ == AF_INET6) {
      return true;
    }
    return false;
  }
  // Comparing addresses of the same family.
  switch (family_) {
    case AF_INET: {
      return NetworkToHost32(u_.ip4.s_addr) <
          NetworkToHost32(other.u_.ip4.s_addr);
    }
    case AF_INET6: {
      return memcmp(&u_.ip6.s6_addr, &other.u_.ip6.s6_addr, 16) < 0;
    }
  }
  // Catches AF_UNSPEC and invalid addresses.
  return false;
}

std::ostream& operator<<(std::ostream& os, const IPAddress& ip) {
  os << ip.ToString();
  return os;
}

in6_addr IPAddress::ipv6_address() const {
  return u_.ip6;
}

in_addr IPAddress::ipv4_address() const {
  return u_.ip4;
}

std::string IPAddress::ToString() const {
  if (family_ != AF_INET && family_ != AF_INET6) {
    return std::string();
  }
  char buf[INET6_ADDRSTRLEN] = {0};
  const void* src = &u_.ip4;
  if (family_ == AF_INET6) {
    src = &u_.ip6;
  }
  if (!rtc::inet_ntop(family_, src, buf, sizeof(buf))) {
    return std::string();
  }
  return std::string(buf);
}

std::string IPAddress::ToSensitiveString() const {
#if !defined(NDEBUG)
  // Return non-stripped in debug.
  return ToString();
#else
  switch (family_) {
    case AF_INET: {
      std::string address = ToString();
      size_t find_pos = address.rfind('.');
      if (find_pos == std::string::npos)
        return std::string();
      address.resize(find_pos);
      address += ".x";
      return address;
    }
    case AF_INET6: {
      // TODO(grunell): Return a string of format 1:2:3:x:x:x:x:x or such
      // instead of zeroing out.
      return TruncateIP(*this, 128 - 80).ToString();
    }
  }
  return std::string();
#endif
}

IPAddress IPAddress::Normalized() const {
  if (family_ != AF_INET6) {
    return *this;
  }
  if (!IPIsV4Mapped(*this)) {
    return *this;
  }
  in_addr addr = ExtractMappedAddress(u_.ip6);
  return IPAddress(addr);
}

IPAddress IPAddress::AsIPv6Address() const {
  if (family_ != AF_INET) {
    return *this;
  }
  in6_addr v6addr = kV4MappedPrefix;
  ::memcpy(&v6addr.s6_addr[12], &u_.ip4.s_addr, sizeof(u_.ip4.s_addr));
  return IPAddress(v6addr);
}

bool InterfaceAddress::operator==(const InterfaceAddress &other) const {
  return ipv6_flags_ == other.ipv6_flags() &&
    static_cast<const IPAddress&>(*this) == other;
}

bool InterfaceAddress::operator!=(const InterfaceAddress &other) const {
  return !((*this) == other);
}

const InterfaceAddress& InterfaceAddress::operator=(
  const InterfaceAddress& other) {
  ipv6_flags_ = other.ipv6_flags_;
  static_cast<IPAddress&>(*this) = other;
  return *this;
}

std::ostream& operator<<(std::ostream& os, const InterfaceAddress& ip) {
  os << static_cast<const IPAddress&>(ip);

  if (ip.family() == AF_INET6)
    os << "|flags:0x" << std::hex << ip.ipv6_flags();

  return os;
}

bool IsPrivateV4(uint32_t ip_in_host_order) {
  return ((ip_in_host_order >> 24) == 127) ||
      ((ip_in_host_order >> 24) == 10) ||
      ((ip_in_host_order >> 20) == ((172 << 4) | 1)) ||
      ((ip_in_host_order >> 16) == ((192 << 8) | 168)) ||
      ((ip_in_host_order >> 16) == ((169 << 8) | 254));
}

in_addr ExtractMappedAddress(const in6_addr& in6) {
  in_addr ipv4;
  ::memcpy(&ipv4.s_addr, &in6.s6_addr[12], sizeof(ipv4.s_addr));
  return ipv4;
}

bool IPFromAddrInfo(struct addrinfo* info, IPAddress* out) {
  if (!info || !info->ai_addr) {
    return false;
  }
  if (info->ai_addr->sa_family == AF_INET) {
    sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(info->ai_addr);
    *out = IPAddress(addr->sin_addr);
    return true;
  } else if (info->ai_addr->sa_family == AF_INET6) {
    sockaddr_in6* addr = reinterpret_cast<sockaddr_in6*>(info->ai_addr);
    *out = IPAddress(addr->sin6_addr);
    return true;
  }
  return false;
}

bool IPFromString(const std::string& str, IPAddress* out) {
  if (!out) {
    return false;
  }
  in_addr addr;
  if (rtc::inet_pton(AF_INET, str.c_str(), &addr) == 0) {
    in6_addr addr6;
    if (rtc::inet_pton(AF_INET6, str.c_str(), &addr6) == 0) {
      *out = IPAddress();
      return false;
    }
    *out = IPAddress(addr6);
  } else {
    *out = IPAddress(addr);
  }
  return true;
}

bool IPFromString(const std::string& str, int flags,
                  InterfaceAddress* out) {
  IPAddress ip;
  if (!IPFromString(str, &ip)) {
    return false;
  }

  *out = InterfaceAddress(ip, flags);
  return true;
}

bool IPIsAny(const IPAddress& ip) {
  switch (ip.family()) {
    case AF_INET:
      return ip == IPAddress(INADDR_ANY);
    case AF_INET6:
      return ip == IPAddress(in6addr_any) || ip == IPAddress(kV4MappedPrefix);
    case AF_UNSPEC:
      return false;
  }
  return false;
}

bool IPIsLoopback(const IPAddress& ip) {
  switch (ip.family()) {
    case AF_INET: {
      return ip == IPAddress(INADDR_LOOPBACK);
    }
    case AF_INET6: {
      return ip == IPAddress(in6addr_loopback);
    }
  }
  return false;
}

bool IPIsPrivate(const IPAddress& ip) {
  switch (ip.family()) {
    case AF_INET: {
      return IsPrivateV4(ip.v4AddressAsHostOrderInteger());
    }
    case AF_INET6: {
      return IPIsLinkLocal(ip) || IPIsLoopback(ip);
    }
  }
  return false;
}

bool IPIsUnspec(const IPAddress& ip) {
  return ip.family() == AF_UNSPEC;
}

size_t HashIP(const IPAddress& ip) {
  switch (ip.family()) {
    case AF_INET: {
      return ip.ipv4_address().s_addr;
    }
    case AF_INET6: {
      in6_addr v6addr = ip.ipv6_address();
      const uint32_t* v6_as_ints =
          reinterpret_cast<const uint32_t*>(&v6addr.s6_addr);
      return v6_as_ints[0] ^ v6_as_ints[1] ^ v6_as_ints[2] ^ v6_as_ints[3];
    }
  }
  return 0;
}

IPAddress TruncateIP(const IPAddress& ip, int length) {
  if (length < 0) {
    return IPAddress();
  }
  if (ip.family() == AF_INET) {
    if (length > 31) {
      return ip;
    }
    if (length == 0) {
      return IPAddress(INADDR_ANY);
    }
    int mask = (0xFFFFFFFF << (32 - length));
    uint32_t host_order_ip = NetworkToHost32(ip.ipv4_address().s_addr);
    in_addr masked;
    masked.s_addr = HostToNetwork32(host_order_ip & mask);
    return IPAddress(masked);
  } else if (ip.family() == AF_INET6) {
    if (length > 127) {
      return ip;
    }
    if (length == 0) {
      return IPAddress(in6addr_any);
    }
    in6_addr v6addr = ip.ipv6_address();
    int position = length / 32;
    int inner_length = 32 - (length - (position * 32));
    // Note: 64bit mask constant needed to allow possible 32-bit left shift.
    uint32_t inner_mask = 0xFFFFFFFFLL << inner_length;
    uint32_t* v6_as_ints = reinterpret_cast<uint32_t*>(&v6addr.s6_addr);
    for (int i = 0; i < 4; ++i) {
      if (i == position) {
        uint32_t host_order_inner = NetworkToHost32(v6_as_ints[i]);
        v6_as_ints[i] = HostToNetwork32(host_order_inner & inner_mask);
      } else if (i > position) {
        v6_as_ints[i] = 0;
      }
    }
    return IPAddress(v6addr);
  }
  return IPAddress();
}

int CountIPMaskBits(IPAddress mask) {
  uint32_t word_to_count = 0;
  int bits = 0;
  switch (mask.family()) {
    case AF_INET: {
      word_to_count = NetworkToHost32(mask.ipv4_address().s_addr);
      break;
    }
    case AF_INET6: {
      in6_addr v6addr = mask.ipv6_address();
      const uint32_t* v6_as_ints =
          reinterpret_cast<const uint32_t*>(&v6addr.s6_addr);
      int i = 0;
      for (; i < 4; ++i) {
        if (v6_as_ints[i] != 0xFFFFFFFF) {
          break;
        }
      }
      if (i < 4) {
        word_to_count = NetworkToHost32(v6_as_ints[i]);
      }
      bits = (i * 32);
      break;
    }
    default: {
      return 0;
    }
  }
  if (word_to_count == 0) {
    return bits;
  }

  // Public domain bit-twiddling hack from:
  // http://graphics.stanford.edu/~seander/bithacks.html
  // Counts the trailing 0s in the word.
  unsigned int zeroes = 32;
  word_to_count &= -static_cast<int32_t>(word_to_count);
  if (word_to_count) zeroes--;
  if (word_to_count & 0x0000FFFF) zeroes -= 16;
  if (word_to_count & 0x00FF00FF) zeroes -= 8;
  if (word_to_count & 0x0F0F0F0F) zeroes -= 4;
  if (word_to_count & 0x33333333) zeroes -= 2;
  if (word_to_count & 0x55555555) zeroes -= 1;

  return bits + (32 - zeroes);
}

bool IPIsHelper(const IPAddress& ip, const in6_addr& tomatch, int length) {
  // Helper method for checking IP prefix matches (but only on whole byte
  // lengths). Length is in bits.
  in6_addr addr = ip.ipv6_address();
  return ::memcmp(&addr, &tomatch, (length >> 3)) == 0;
}

bool IPIs6Bone(const IPAddress& ip) {
  return IPIsHelper(ip, k6BonePrefix, 16);
}

bool IPIs6To4(const IPAddress& ip) {
  return IPIsHelper(ip, k6To4Prefix, 16);
}

bool IPIsLinkLocal(const IPAddress& ip) {
  // Can't use the helper because the prefix is 10 bits.
  in6_addr addr = ip.ipv6_address();
  return addr.s6_addr[0] == 0xFE && addr.s6_addr[1] == 0x80;
}

// According to http://www.ietf.org/rfc/rfc2373.txt, Appendix A, page 19.  An
// address which contains MAC will have its 11th and 12th bytes as FF:FE as well
// as the U/L bit as 1.
bool IPIsMacBased(const IPAddress& ip) {
  in6_addr addr = ip.ipv6_address();
  return ((addr.s6_addr[8] & 0x02) && addr.s6_addr[11] == 0xFF &&
          addr.s6_addr[12] == 0xFE);
}

bool IPIsSiteLocal(const IPAddress& ip) {
  // Can't use the helper because the prefix is 10 bits.
  in6_addr addr = ip.ipv6_address();
  return addr.s6_addr[0] == 0xFE && (addr.s6_addr[1] & 0xC0) == 0xC0;
}

bool IPIsULA(const IPAddress& ip) {
  // Can't use the helper because the prefix is 7 bits.
  in6_addr addr = ip.ipv6_address();
  return (addr.s6_addr[0] & 0xFE) == 0xFC;
}

bool IPIsTeredo(const IPAddress& ip) {
  return IPIsHelper(ip, kTeredoPrefix, 32);
}

bool IPIsV4Compatibility(const IPAddress& ip) {
  return IPIsHelper(ip, kV4CompatibilityPrefix, 96);
}

bool IPIsV4Mapped(const IPAddress& ip) {
  return IPIsHelper(ip, kV4MappedPrefix, 96);
}

int IPAddressPrecedence(const IPAddress& ip) {
  // Precedence values from RFC 3484-bis. Prefers native v4 over 6to4/Teredo.
  if (ip.family() == AF_INET) {
    return 30;
  } else if (ip.family() == AF_INET6) {
    if (IPIsLoopback(ip)) {
      return 60;
    } else if (IPIsULA(ip)) {
      return 50;
    } else if (IPIsV4Mapped(ip)) {
      return 30;
    } else if (IPIs6To4(ip)) {
      return 20;
    } else if (IPIsTeredo(ip)) {
      return 10;
    } else if (IPIsV4Compatibility(ip) || IPIsSiteLocal(ip) || IPIs6Bone(ip)) {
      return 1;
    } else {
      // A 'normal' IPv6 address.
      return 40;
    }
  }
  return 0;
}

IPAddress GetLoopbackIP(int family) {
  if (family == AF_INET) {
    return rtc::IPAddress(INADDR_LOOPBACK);
  }
  if (family == AF_INET6) {
    return rtc::IPAddress(in6addr_loopback);
  }
  return rtc::IPAddress();
}

IPAddress GetAnyIP(int family) {
  if (family == AF_INET) {
    return rtc::IPAddress(INADDR_ANY);
  }
  if (family == AF_INET6) {
    return rtc::IPAddress(in6addr_any);
  }
  return rtc::IPAddress();
}

}  // Namespace rtc

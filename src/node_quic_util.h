#ifndef SRC_NODE_QUIC_UTIL_H_
#define SRC_NODE_QUIC_UTIL_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "node.h"
#include "node_quic_buffer.h"
#include "string_bytes.h"
#include "uv.h"
#include "v8.h"

#include <ngtcp2/ngtcp2.h>
#include <openssl/ssl.h>

#include <functional>
#include <limits>
#include <string>

namespace node {
namespace quic {

constexpr uint64_t NGTCP2_APP_NOERROR = 0xff00;

constexpr size_t MIN_INITIAL_QUIC_PKT_SIZE = 1200;
constexpr size_t NGTCP2_SV_SCIDLEN = NGTCP2_MAX_CIDLEN;
constexpr size_t TOKEN_RAND_DATALEN = 16;
constexpr size_t TOKEN_SECRETLEN = 16;

constexpr size_t kMaxSizeT = std::numeric_limits<size_t>::max();
constexpr size_t DEFAULT_MAX_CONNECTIONS_PER_HOST = 100;
constexpr uint64_t MIN_MAX_CRYPTO_BUFFER = 4096;
constexpr uint64_t MIN_RETRYTOKEN_EXPIRATION = 1;
constexpr uint64_t MAX_RETRYTOKEN_EXPIRATION = 60;
constexpr uint64_t DEFAULT_MAX_CRYPTO_BUFFER = MIN_MAX_CRYPTO_BUFFER * 4;
constexpr uint64_t DEFAULT_ACTIVE_CONNECTION_ID_LIMIT = 10;
constexpr uint64_t DEFAULT_MAX_STREAM_DATA_BIDI_LOCAL = 256 * 1024;
constexpr uint64_t DEFAULT_MAX_STREAM_DATA_BIDI_REMOTE = 256 * 1024;
constexpr uint64_t DEFAULT_MAX_STREAM_DATA_UNI = 256 * 1024;
constexpr uint64_t DEFAULT_MAX_DATA = 1 * 1024 * 1024;
constexpr uint64_t DEFAULT_MAX_STREAMS_BIDI = 100;
constexpr uint64_t DEFAULT_MAX_STREAMS_UNI = 3;
constexpr uint64_t DEFAULT_IDLE_TIMEOUT = 10 * 10000000000;
constexpr uint64_t DEFAULT_RETRYTOKEN_EXPIRATION = 10ULL;

enum SelectPreferredAddressPolicy : int {
  // Ignore the server-provided preferred address
  QUIC_PREFERRED_ADDRESS_IGNORE,
  // Accept the server-provided preferred address
  QUIC_PREFERRED_ADDRESS_ACCEPT
};

// Fun hash combine trick based on a variadic template that
// I came across a while back but can't remember where. Will add an attribution
// if I can find the source.
inline void hash_combine(size_t* seed) { }

template <typename T, typename... Args>
inline void hash_combine(size_t* seed, const T& value, Args... rest) {
    *seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (*seed << 6) + (*seed >> 2);
    hash_combine(seed, rest...);
}

// QUIC error codes generally fall into two distinct namespaces:
// Connection Errors and Application Errors. Connection errors
// are further subdivided into Crypto and non-Crypto. Application
// errors are entirely specific to the QUIC application being
// used. An easy rule of thumb is that Application errors are
// semantically associated with the ALPN identifier negotiated
// for the QuicSession. So, if a connection is closed with
// family: QUIC_ERROR_APPLICATION and code: 123, you have to
// look at the ALPN identifier to determine exactly what it
// means. Connection (Session) and Crypto errors, on the other
// hand, share the same meaning regardless of the ALPN.
enum QuicErrorFamily : int32_t {
  QUIC_ERROR_SESSION,
  QUIC_ERROR_CRYPTO,
  QUIC_ERROR_APPLICATION
};

struct QuicError {
  int32_t family;
  uint64_t code;
  QuicError(
      int32_t family_ = QUIC_ERROR_SESSION,
      int code_ = NGTCP2_NO_ERROR);
  QuicError(
      int32_t family_ = QUIC_ERROR_SESSION,
      uint64_t code_ = NGTCP2_NO_ERROR);
  QuicError(
      Environment* env,
      v8::Local<v8::Value> codeArg,
      v8::Local<v8::Value> familyArg = v8::Local<v8::Object>(),
      int32_t family_ = QUIC_ERROR_SESSION);
  const char* GetFamilyName();
};

class SocketAddress {
 public:
  // std::hash specialization for sockaddr instances (ipv4 or ipv6) used
  // for tracking the number of connections per client.
  struct Hash {
    size_t operator()(const sockaddr* addr) const {
      size_t hash = 0;
      switch (addr->sa_family) {
        case AF_INET: {
          const sockaddr_in* ipv4 =
              reinterpret_cast<const sockaddr_in*>(addr);
          hash_combine(&hash, ipv4->sin_port, ipv4->sin_addr.s_addr);
          break;
        }
        case AF_INET6: {
          const sockaddr_in6* ipv6 =
              reinterpret_cast<const sockaddr_in6*>(addr);
          const uint64_t* a =
              reinterpret_cast<const uint64_t*>(&ipv6->sin6_addr);
          hash_combine(&hash, ipv6->sin6_port, a[0], a[1]);
          break;
        }
        default:
          UNREACHABLE();
      }
      return hash;
    }
  };

  // std::equal_to specialization for sockaddr instances (ipv4 or ipv6).
  struct Compare {
    bool operator()(const sockaddr* laddr, const sockaddr* raddr) const {
      CHECK(laddr->sa_family == AF_INET || laddr->sa_family == AF_INET6);
      return memcmp(laddr, raddr, GetAddressLen(laddr)) == 0;
    }
  };

  static bool numeric_host(const char* hostname) {
    return numeric_host(hostname, AF_INET) || numeric_host(hostname, AF_INET6);
  }

  static bool numeric_host(const char* hostname, int family) {
    std::array<uint8_t, sizeof(struct in6_addr)> dst;
    return inet_pton(family, hostname, dst.data()) == 1;
  }

  static size_t GetMaxPktLen(const sockaddr* addr) {
    return addr->sa_family == AF_INET6 ?
        NGTCP2_MAX_PKTLEN_IPV6 :
        NGTCP2_MAX_PKTLEN_IPV4;
  }

  static bool ResolvePreferredAddress(
      Environment* env,
      int local_address_family,
      const ngtcp2_preferred_addr* paddr,
      uv_getaddrinfo_t* req) {
    int af;
    const uint8_t* binaddr;
    uint16_t port;
    constexpr uint8_t empty_addr[] = {0, 0, 0, 0, 0, 0, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0};

    if (local_address_family == AF_INET &&
        memcmp(empty_addr, paddr->ipv4_addr, sizeof(paddr->ipv4_addr)) != 0) {
      af = AF_INET;
      binaddr = paddr->ipv4_addr;
      port = paddr->ipv4_port;
    } else if (local_address_family == AF_INET6 &&
               memcmp(empty_addr,
                      paddr->ipv6_addr,
                      sizeof(paddr->ipv6_addr)) != 0) {
      af = AF_INET6;
      binaddr = paddr->ipv6_addr;
      port = paddr->ipv6_port;
    } else {
      return false;
    }

    char host[NI_MAXHOST];
    if (uv_inet_ntop(af, binaddr, host, sizeof(host)) != 0)
      return false;

    addrinfo hints{};
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    hints.ai_family = af;
    hints.ai_socktype = SOCK_DGRAM;

    return
        uv_getaddrinfo(
            env->event_loop(),
            req,
            nullptr,
            host,
            std::to_string(port).c_str(),
            &hints) == 0;
  }

  static int ToSockAddr(
      int32_t family,
      const char* host,
      uint32_t port,
      sockaddr_storage* addr) {
    CHECK(family == AF_INET || family == AF_INET6);
    switch (family) {
       case AF_INET:
        return uv_ip4_addr(host, port, reinterpret_cast<sockaddr_in*>(addr));
      case AF_INET6:
        return uv_ip6_addr(host, port, reinterpret_cast<sockaddr_in6*>(addr));
       default:
        CHECK(0 && "unexpected address family");
    }
  }

  static int GetPort(const sockaddr* addr) {
    return ntohs(addr->sa_family == AF_INET ?
        reinterpret_cast<const sockaddr_in*>(addr)->sin_port :
        reinterpret_cast<const sockaddr_in6*>(addr)->sin6_port);
  }

  static void GetAddress(const sockaddr* addr, char* host, size_t host_len) {
    const void* src = addr->sa_family == AF_INET ?
        static_cast<const void*>(
            &(reinterpret_cast<const sockaddr_in*>(addr)->sin_addr)) :
        static_cast<const void*>(
            &(reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr));
    uv_inet_ntop(addr->sa_family, src, host, host_len);
  }

  static size_t GetAddressLen(const sockaddr* addr) {
    return
        addr->sa_family == AF_INET6 ?
            sizeof(sockaddr_in6) :
            sizeof(sockaddr_in);
  }

  static size_t GetAddressLen(const sockaddr_storage* addr) {
    return
        addr->ss_family == AF_INET6 ?
            sizeof(sockaddr_in6) :
            sizeof(sockaddr_in);
  }

  void Copy(SocketAddress* addr) {
    Copy(**addr);
  }

  void Copy(const sockaddr* source) {
    memcpy(&address_, source, GetAddressLen(source));
  }

  void Update(const ngtcp2_addr* addr) {
    memcpy(&address_, addr->addr, addr->addrlen);
  }

  const sockaddr* operator*() const {
    return reinterpret_cast<const sockaddr*>(&address_);
  }

  ngtcp2_addr ToAddr() {
    return ngtcp2_addr{Size(), reinterpret_cast<uint8_t*>(&address_), nullptr};
  }

  size_t Size() {
    return GetAddressLen(&address_);
  }

  int GetFamily() { return address_.ss_family; }

 private:
  sockaddr_storage address_;
};

struct QuicPath : public ngtcp2_path {
  QuicPath(
    SocketAddress* local,
    SocketAddress* remote) {
    ngtcp2_addr_init(&this->local, **local, local->Size(), nullptr);
    ngtcp2_addr_init(&this->remote, **remote, remote->Size(), nullptr);
  }
};

struct QuicPathStorage : public ngtcp2_path_storage {
  QuicPathStorage() {
    ngtcp2_path_storage_zero(this);
  }
};

// Simple wrapper for ngtcp2_cid that handles hex encoding
// and conversion to std::string automatically
class QuicCID {
 public:
  explicit QuicCID(ngtcp2_cid* cid) :
      cid_(*cid),
      str_(cid->data, cid->data + cid->datalen) {}
  explicit QuicCID(const ngtcp2_cid* cid) :
      cid_(*cid),
      str_(cid->data, cid->data + cid->datalen) {}
  explicit QuicCID(const ngtcp2_cid& cid) :
      cid_(cid),
      str_(cid.data, cid.data + cid.datalen) {}
  QuicCID(const uint8_t* cid, size_t len) {
    ngtcp2_cid_init(&cid_, cid, len);
    str_ = std::string(cid_.data, cid_.data + cid_.datalen);
  }

  std::string ToStr() const { return str_; }

  std::string ToHex() const {
    if (hex_.empty() && cid_.datalen > 0) {
      size_t len = cid_.datalen * 2;
      MaybeStackBuffer<char, 64> dest;
      dest.AllocateSufficientStorage(len);
      dest.SetLengthAndZeroTerminate(len);
      size_t written = StringBytes::hex_encode(
          reinterpret_cast<const char*>(cid_.data),
          cid_.datalen,
          *dest,
          dest.length());
      hex_ = std::string(*dest, written);
    }
    return hex_;
  }

  const ngtcp2_cid* operator*() const { return &cid_; }

  const uint8_t* data() const { return cid_.data; }
  size_t length() const { return cid_.datalen; }

 private:
  ngtcp2_cid cid_;
  std::string str_;
  mutable std::string hex_;
};

// https://stackoverflow.com/questions/33701430/template-function-to-access-struct-members
template <typename C, typename T>
decltype(auto) access(C* cls, T C::*member) {
  return (cls->*member);
}

template <typename C, typename T, typename... Mems>
decltype(auto) access(C* cls, T C::*member, Mems... rest) {
  return access((cls->*member), rest...);
}

template <typename A, typename... Members>
void IncrementStat(
    uint64_t amount,
    A* a,
    Members... mems) {
  static uint64_t max = std::numeric_limits<uint64_t>::max();
  uint64_t current = access(a, mems...);
  uint64_t delta = std::min(amount, max - current);
  access(a, mems...) += delta;
}

// Simple timer wrapper that is used to implement the internals
// for idle and retransmission timeouts. Call Update to start or
// reset the timer; Stop to halt the timer.
class Timer final : public MemoryRetainer {
 public:
  explicit Timer(Environment* env, std::function<void()> fn)
    : env_(env),
      fn_(fn) {
    uv_timer_init(env_->event_loop(), &timer_);
    timer_.data = this;
  }

  // Stops the timer with the side effect of the timer no longer being usable.
  // It will be cleaned up and the Timer object will be destroyed.
  void Stop() {
    if (stopped_)
      return;
    stopped_ = true;

    if (timer_.data == this) {
      uv_timer_stop(&timer_);
      timer_.data = nullptr;
    }
  }

  // If the timer is not currently active, interval must be either 0 or greater.
  // If the timer is already active, interval is ignored.
  void Update(uint64_t interval) {
    if (stopped_)
      return;
    uv_timer_start(&timer_, OnTimeout, interval, interval);
    uv_unref(reinterpret_cast<uv_handle_t*>(&timer_));
  }

  static void Free(Timer* timer);

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(Timer)
  SET_SELF_SIZE(Timer)

 private:
  static void OnTimeout(uv_timer_t* timer);

  bool stopped_ = false;
  Environment* env_;
  std::function<void()> fn_;
  uv_timer_t timer_;
};

using TimerPointer = DeleteFnPtr<Timer, Timer::Free>;

ngtcp2_crypto_level from_ossl_level(OSSL_ENCRYPTION_LEVEL ossl_level);
const char* crypto_level_name(ngtcp2_crypto_level level);

}  // namespace quic
}  // namespace node

#endif  // NOE_WANT_INTERNALS

#endif  // SRC_NODE_QUIC_UTIL_H_

#include "cachegrid/Poller.h"

#include <cerrno>
#include <system_error>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define CACHEGRID_KQUEUE 1
#include <sys/event.h>
#elif defined(__linux__)
#define CACHEGRID_EPOLL 1
#include <sys/epoll.h>
#else
#error "CacheGrid needs kqueue (macOS/BSD) or epoll (Linux)"
#endif

namespace cachegrid {

namespace {
[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}
}  // namespace

#if CACHEGRID_KQUEUE
// ---- kqueue ------------------------------------------------------------------
// kqueue tracks (fd, filter) pairs. A socket gets a READ filter and a WRITE
// filter; the WRITE filter is registered disabled and switched on only when
// there is pending output.

Poller::Poller() : fd_(::kqueue()) {
  if (fd_ < 0) throw_errno("kqueue");
}

Poller::~Poller() { ::close(fd_); }

void Poller::add(int fd) {
  struct kevent ch[2];
  EV_SET(&ch[0], fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
  EV_SET(&ch[1], fd, EVFILT_WRITE, EV_ADD | EV_DISABLE, 0, 0, nullptr);
  if (::kevent(fd_, ch, 2, nullptr, 0, nullptr) < 0) throw_errno("kevent add");
}

void Poller::add_readonly(int fd, bool /*shared*/) {
  struct kevent ch;
  EV_SET(&ch, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
  if (::kevent(fd_, &ch, 1, nullptr, 0, nullptr) < 0) throw_errno("kevent add");
}

void Poller::set_interest(int fd, bool read, bool write) {
  struct kevent ch[2];
  EV_SET(&ch[0], fd, EVFILT_READ, read ? EV_ENABLE : EV_DISABLE, 0, 0, nullptr);
  EV_SET(&ch[1], fd, EVFILT_WRITE, write ? EV_ENABLE : EV_DISABLE, 0, 0, nullptr);
  if (::kevent(fd_, ch, 2, nullptr, 0, nullptr) < 0) throw_errno("kevent set interest");
}

void Poller::remove(int /*fd*/) {
  // Nothing to do: the kernel drops all of an fd's kqueue filters
  // automatically when the fd is closed.
}

int Poller::wait(std::vector<PollEvent>& out, int max_events, int timeout_ms) {
  // Reused across calls: this is the hottest loop in the server.
  thread_local std::vector<struct kevent> evs;
  evs.resize(static_cast<std::size_t>(max_events));
  struct timespec ts;
  struct timespec* tsp = nullptr;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
    tsp = &ts;
  }
  const int n = ::kevent(fd_, nullptr, 0, evs.data(), max_events, tsp);
  if (n < 0) {
    if (errno == EINTR) return 0;
    throw_errno("kevent wait");
  }
  out.clear();
  for (int i = 0; i < n; ++i) {
    PollEvent e;
    e.fd = static_cast<int>(evs[static_cast<std::size_t>(i)].ident);
    const auto filter = evs[static_cast<std::size_t>(i)].filter;
    const auto flags = evs[static_cast<std::size_t>(i)].flags;
    e.readable = (filter == EVFILT_READ) || (flags & EV_ERROR);
    e.writable = (filter == EVFILT_WRITE);
    out.push_back(e);
  }
  return n;
}

#elif CACHEGRID_EPOLL
// ---- epoll -------------------------------------------------------------------

Poller::Poller() : fd_(::epoll_create1(EPOLL_CLOEXEC)) {
  if (fd_ < 0) throw_errno("epoll_create1");
}

Poller::~Poller() { ::close(fd_); }

void Poller::add(int fd) {
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if (::epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &ev) < 0) throw_errno("epoll_ctl add");
}

void Poller::add_readonly(int fd, bool shared) {
  epoll_event ev{};
  ev.events = EPOLLIN;
#ifdef EPOLLEXCLUSIVE
  if (shared) ev.events |= EPOLLEXCLUSIVE;
#else
  (void)shared;
#endif
  ev.data.fd = fd;
  if (::epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &ev) < 0) throw_errno("epoll_ctl add");
}

void Poller::set_interest(int fd, bool read, bool write) {
  epoll_event ev{};
  ev.events = (read ? EPOLLIN : 0u) | (write ? EPOLLOUT : 0u);
  ev.data.fd = fd;
  if (::epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &ev) < 0) throw_errno("epoll_ctl mod");
}

void Poller::remove(int fd) {
  ::epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr);  // failure is harmless here
}

int Poller::wait(std::vector<PollEvent>& out, int max_events, int timeout_ms) {
  // Reused across calls: this is the hottest loop in the server.
  thread_local std::vector<epoll_event> evs;
  evs.resize(static_cast<std::size_t>(max_events));
  const int n = ::epoll_wait(fd_, evs.data(), max_events, timeout_ms);
  if (n < 0) {
    if (errno == EINTR) return 0;
    throw_errno("epoll_wait");
  }
  out.clear();
  for (int i = 0; i < n; ++i) {
    const auto& ev = evs[static_cast<std::size_t>(i)];
    PollEvent e;
    e.fd = ev.data.fd;
    e.readable = (ev.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
    e.writable = (ev.events & EPOLLOUT) != 0;
    out.push_back(e);
  }
  return n;
}
#endif

}  // namespace cachegrid

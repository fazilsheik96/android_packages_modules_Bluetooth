/******************************************************************************
 *
 *  Copyright 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/*******************************************************************************
 *
 *  Filename:      btif_sock_thread.cc
 *
 *  Description:   socket select thread
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_sock"

#include "btif_sock_thread.h"

#include <alloca.h>
#include <bluetooth/log.h>
#include <fcntl.h>
#include <features.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <mutex>
#include <optional>

#include "os/log.h"
#include "osi/include/osi.h"  // OSI_NO_INTR

#define asrt(s)                                         \
  do {                                                  \
    if (!(s)) log::error("## assert {} failed ##", #s); \
  } while (0)

#define MAX_THREAD 8
#define MAX_POLL 64
#define POLL_EXCEPTION_EVENTS (POLLHUP | POLLRDHUP | POLLERR | POLLNVAL)
#define IS_EXCEPTION(e) ((e)&POLL_EXCEPTION_EVENTS)
#define IS_READ(e) ((e)&POLLIN)
#define IS_WRITE(e) ((e)&POLLOUT)
/*cmd executes in socket poll thread */
#define CMD_WAKEUP 1
#define CMD_EXIT 2
#define CMD_ADD_FD 3
#define CMD_REMOVE_FD 4
#define CMD_USER_PRIVATE 5

using namespace bluetooth;

struct poll_slot_t {
  struct pollfd pfd;
  uint32_t user_id;
  int type;
  int flags;
};
struct thread_slot_t {
  int cmd_fdr, cmd_fdw;
  int poll_count;
  poll_slot_t ps[MAX_POLL];
  int psi[MAX_POLL];  // index of poll slot
  std::optional<pthread_t> thread_id;
  btsock_signaled_cb callback;
  btsock_cmd_cb cmd_callback;
  int used;
};
static thread_slot_t ts[MAX_THREAD];

static void* sock_poll_thread(void* arg);
static inline void close_cmd_fd(int h);

static inline void add_poll(int h, int fd, int type, int flags,
                            uint32_t user_id);

static std::recursive_mutex thread_slot_lock;

static inline int create_thread(void* (*start_routine)(void*), void* arg,
                                pthread_t* thread_id) {
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);
  pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);
  int policy;
  int min_pri = 0;
  int ret = -1;
  struct sched_param param;

  ret = pthread_create(thread_id, &thread_attr, start_routine, arg);
  if (ret != 0) {
    log::error("pthread_create : {}", strerror(errno));
    return ret;
  }
  /* We need to lower the priority of this thread to ensure the stack gets
   * priority over transfer to a socket */
  pthread_getschedparam(*thread_id, &policy, &param);
  min_pri = sched_get_priority_min(policy);
  if (param.sched_priority > min_pri) {
    param.sched_priority -= 1;
  }
  pthread_setschedparam(*thread_id, policy, &param);
  return ret;
}
static void init_poll(int cmd_fd);
static int alloc_thread_slot() {
  std::unique_lock<std::recursive_mutex> lock(thread_slot_lock);
  int i;
  // reversed order to save guard uninitialized access to 0 index
  for (i = MAX_THREAD - 1; i >= 0; i--) {
    if (!ts[i].used) {
      ts[i].used = 1;
      return i;
    }
  }
  log::error("execeeded max thread count");
  return -1;
}
static void free_thread_slot(int h) {
  if (0 <= h && h < MAX_THREAD) {
    close_cmd_fd(h);
    ts[h].used = 0;
  } else
    log::error("invalid thread handle:{}", h);
}
void btsock_thread_init() {
  static int initialized;
  if (!initialized) {
    initialized = 1;
    int h;
    for (h = 0; h < MAX_THREAD; h++) {
      ts[h].cmd_fdr = ts[h].cmd_fdw = -1;
      ts[h].used = 0;
      ts[h].thread_id = std::nullopt;
      ts[h].poll_count = 0;
      ts[h].callback = NULL;
      ts[h].cmd_callback = NULL;
    }
  }
}
int btsock_thread_create(btsock_signaled_cb callback,
                         btsock_cmd_cb cmd_callback) {
  asrt(callback || cmd_callback);
  int h = alloc_thread_slot();
  if (h >= 0) {
    init_poll(h);
    pthread_t thread;
    int status = create_thread(sock_poll_thread, (void*)(uintptr_t)h, &thread);
    if (status) {
      log::error("create_thread failed: {}", strerror(status));
      free_thread_slot(h);
      return -1;
    }

    ts[h].thread_id = thread;
    ts[h].callback = callback;
    ts[h].cmd_callback = cmd_callback;
  }
  return h;
}

/* create dummy socket pair used to wake up select loop */
static inline void init_cmd_fd(int h) {
  asrt(ts[h].cmd_fdr == -1 && ts[h].cmd_fdw == -1);
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, &ts[h].cmd_fdr) < 0) {
    log::error("socketpair failed: {}", strerror(errno));
    return;
  }
  // add the cmd fd for read & write
  add_poll(h, ts[h].cmd_fdr, 0, SOCK_THREAD_FD_RD, 0);
}
static inline void close_cmd_fd(int h) {
  if (ts[h].cmd_fdr != -1) {
    close(ts[h].cmd_fdr);
    ts[h].cmd_fdr = -1;
  }
  if (ts[h].cmd_fdw != -1) {
    close(ts[h].cmd_fdw);
    ts[h].cmd_fdw = -1;
  }
}
typedef struct {
  int id;
  int fd;
  int type;
  int flags;
  uint32_t user_id;
} sock_cmd_t;
int btsock_thread_add_fd(int h, int fd, int type, int flags, uint32_t user_id) {
  if (h < 0 || h >= MAX_THREAD) {
    log::error("invalid bt thread handle:{}", h);
    return false;
  }
  if (ts[h].cmd_fdw == -1) {
    log::error("cmd socket is not created. socket thread may not initialized");
    return false;
  }
  if (flags & SOCK_THREAD_ADD_FD_SYNC) {
    // must executed in socket poll thread
    if (ts[h].thread_id.value() == pthread_self()) {
      // cleanup one-time flags
      flags &= ~SOCK_THREAD_ADD_FD_SYNC;
      add_poll(h, fd, type, flags, user_id);
      return true;
    }
    log::warn(
        "THREAD_ADD_FD_SYNC is not called in poll thread, fallback to async");
  }
  sock_cmd_t cmd = {CMD_ADD_FD, fd, type, flags, user_id};

  ssize_t ret;
  OSI_NO_INTR(ret = send(ts[h].cmd_fdw, &cmd, sizeof(cmd), 0));

  return ret == sizeof(cmd);
}
int btsock_thread_wakeup(int h) {
  if (h < 0 || h >= MAX_THREAD) {
    log::error("invalid bt thread handle:{}", h);
    return false;
  }
  if (ts[h].cmd_fdw == -1) {
    log::error("thread handle:{}, cmd socket is not created", h);
    return false;
  }
  sock_cmd_t cmd = {CMD_WAKEUP, 0, 0, 0, 0};

  ssize_t ret;
  OSI_NO_INTR(ret = send(ts[h].cmd_fdw, &cmd, sizeof(cmd), 0));

  return ret == sizeof(cmd);
}
int btsock_thread_exit(int h) {
  if (h < 0 || h >= MAX_THREAD) {
    log::error("invalid bt thread slot:{}", h);
    return false;
  }
  if (ts[h].cmd_fdw == -1) {
    log::error("cmd socket is not created");
    return false;
  }
  sock_cmd_t cmd = {CMD_EXIT, 0, 0, 0, 0};

  ssize_t ret;
  OSI_NO_INTR(ret = send(ts[h].cmd_fdw, &cmd, sizeof(cmd), 0));

  if (ret == sizeof(cmd)) {
    if (ts[h].thread_id != std::nullopt) {
      pthread_join(ts[h].thread_id.value(), 0);
      ts[h].thread_id = std::nullopt;
    }
    free_thread_slot(h);
    return true;
  }
  return false;
}
static void init_poll(int h) {
  int i;
  ts[h].poll_count = 0;
  ts[h].thread_id = std::nullopt;
  ts[h].callback = NULL;
  ts[h].cmd_callback = NULL;
  for (i = 0; i < MAX_POLL; i++) {
    ts[h].ps[i].pfd.fd = -1;
    ts[h].psi[i] = -1;
  }
  init_cmd_fd(h);
}
static inline unsigned int flags2pevents(int flags) {
  unsigned int pevents = 0;
  if (flags & SOCK_THREAD_FD_WR) pevents |= POLLOUT;
  if (flags & SOCK_THREAD_FD_RD) pevents |= POLLIN;
  pevents |= POLL_EXCEPTION_EVENTS;
  return pevents;
}

static inline void set_poll(poll_slot_t* ps, int fd, int type, int flags,
                            uint32_t user_id) {
  ps->pfd.fd = fd;
  ps->user_id = user_id;
  if (ps->type != 0 && ps->type != type)
    log::error("poll socket type should not changed! type was:{}, type now:{}",
               ps->type, type);
  ps->type = type;
  ps->flags = flags;
  ps->pfd.events = flags2pevents(flags);
  ps->pfd.revents = 0;
}
static inline void add_poll(int h, int fd, int type, int flags,
                            uint32_t user_id) {
  asrt(fd != -1);
  int i;
  int empty = -1;
  poll_slot_t* ps = ts[h].ps;

  for (i = 0; i < MAX_POLL; i++) {
    if (ps[i].pfd.fd == fd) {
      asrt(ts[h].poll_count < MAX_POLL);

      set_poll(&ps[i], fd, type, flags | ps[i].flags, user_id);
      return;
    } else if (empty < 0 && ps[i].pfd.fd == -1)
      empty = i;
  }
  if (empty >= 0) {
    asrt(ts[h].poll_count < MAX_POLL);
    set_poll(&ps[empty], fd, type, flags, user_id);
    ++ts[h].poll_count;
    return;
  }
  log::error("exceeded max poll slot:{}!", MAX_POLL);
}
static inline void remove_poll(int h, poll_slot_t* ps, int flags) {
  if (flags == ps->flags) {
    // all monitored events signaled. To remove it, just clear the slot
    --ts[h].poll_count;
    memset(ps, 0, sizeof(*ps));
    ps->pfd.fd = -1;
  } else {
    // one read or one write monitor event signaled, removed the accordding bit
    ps->flags &= ~flags;
    // update the poll events mask
    ps->pfd.events = flags2pevents(ps->flags);
  }
}
static int process_cmd_sock(int h) {
  sock_cmd_t cmd = {-1, 0, 0, 0, 0};
  int fd = ts[h].cmd_fdr;

  ssize_t ret;
  OSI_NO_INTR(ret = recv(fd, &cmd, sizeof(cmd), MSG_WAITALL));

  if (ret != sizeof(cmd)) {
    log::error("recv cmd errno:{}", errno);
    return false;
  }
  switch (cmd.id) {
    case CMD_ADD_FD:
      add_poll(h, cmd.fd, cmd.type, cmd.flags, cmd.user_id);
      break;
    case CMD_REMOVE_FD:
      for (int i = 1; i < MAX_POLL; ++i) {
        poll_slot_t* poll_slot = &ts[h].ps[i];
        if (poll_slot->pfd.fd == cmd.fd) {
          remove_poll(h, poll_slot, poll_slot->flags);
          break;
        }
      }
      close(cmd.fd);
      break;
    case CMD_WAKEUP:
      break;
    case CMD_USER_PRIVATE:
      asrt(ts[h].cmd_callback);
      if (ts[h].cmd_callback)
        ts[h].cmd_callback(fd, cmd.type, cmd.flags, cmd.user_id);
      break;
    case CMD_EXIT:
      return false;
    default:
      log::warn("unknown cmd: {}", cmd.id);
      break;
  }
  return true;
}

static void process_data_sock(int h, struct pollfd* pfds, int pfds_count,
                              int event_count) {
  asrt(event_count <= pfds_count);
  int i;
  for (i = 1; i < pfds_count; i++) {
    if (pfds[i].revents) {
      int ps_i = ts[h].psi[i];
      if (ts[h].ps[ps_i].pfd.fd == -1) {
        log::info("Socket has been removed from poll set");
        continue;
      }
      asrt(pfds[i].fd == ts[h].ps[ps_i].pfd.fd);
      uint32_t user_id = ts[h].ps[ps_i].user_id;
      int type = ts[h].ps[ps_i].type;
      int flags = 0;
      if (IS_READ(pfds[i].revents)) {
        flags |= SOCK_THREAD_FD_RD;
      }
      if (IS_WRITE(pfds[i].revents)) {
        flags |= SOCK_THREAD_FD_WR;
      }
      if (IS_EXCEPTION(pfds[i].revents)) {
        flags |= SOCK_THREAD_FD_EXCEPTION;
        // remove the whole slot not flags
        remove_poll(h, &ts[h].ps[ps_i], ts[h].ps[ps_i].flags);
      } else if (flags)
        remove_poll(h, &ts[h].ps[ps_i],
                    flags);  // remove the monitor flags that already processed
      if (flags) ts[h].callback(pfds[i].fd, type, flags, user_id);
    }
  }
}

static void prepare_poll_fds(int h, struct pollfd* pfds) {
  int count = 0;
  int ps_i = 0;
  int pfd_i = 0;
  asrt(ts[h].poll_count <= MAX_POLL);
  while (count < ts[h].poll_count) {
    if (ps_i >= MAX_POLL) {
      log::error(
          "exceed max poll range, ps_i:{}, MAX_POLL:{}, count:{}, "
          "ts[h].poll_count:{}",
          ps_i, MAX_POLL, count, ts[h].poll_count);
      return;
    }
    if (ts[h].ps[ps_i].pfd.fd >= 0) {
      pfds[pfd_i] = ts[h].ps[ps_i].pfd;
      ts[h].psi[pfd_i] = ps_i;
      count++;
      pfd_i++;
    }
    ps_i++;
  }
}
static void* sock_poll_thread(void* arg) {
  std::array<struct pollfd, MAX_POLL> pfds;
  if (pthread_setname_np(pthread_self(), "btif_sock_poll") != 0) {
    log::error("set thread name=btif_sock_poll failed");
  }

  int h = (intptr_t)arg;
  for (;;) {
    pfds = {};
    prepare_poll_fds(h, pfds.data());
    int ret;
    OSI_NO_INTR(ret = poll(pfds.data(), ts[h].poll_count, -1));
    if (ret == -1) {
      log::error("poll ret -1, exit the thread, errno:{}, err:{}", errno,
                 strerror(errno));
      break;
    }
    if (ret != 0) {
      int need_process_data_fd = true;
      int pfds_count = ts[h].poll_count;
      if (pfds[0].revents)  // cmd fd always is the first one
      {
        asrt(pfds[0].fd == ts[h].cmd_fdr);
        if (!process_cmd_sock(h)) {
          log::info("h:{}, process_cmd_sock return false, exit...", h);
          break;
        }
        if (ret == 1)
          need_process_data_fd = false;
        else
          ret--;  // exclude the cmd fd
      }
      if (need_process_data_fd)
        process_data_sock(h, pfds.data(), pfds_count, ret);
    } else {
      log::info("no data, select ret: {}", ret);
    };
  }
  log::info("socket poll thread exiting, h:{}", h);
  return 0;
}

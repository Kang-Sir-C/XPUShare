/**
 * Copyright 2020 Hung-Hsin Chen, LSA Lab, National Tsing Hua University
 * Copyright 2025 XPUShare Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

/**
 * Per-GPU time-slice scheduler (xhook-schd) — v2 rewrite.
 *
 * Key fixes over the v1 scheduler:
 *
 *   1. Usage accounting: replaced the overlap-splitting heuristic with
 *      direct per-client summation of history intervals clipped to the
 *      current window.  The old code divided overlapping intervals by
 *      overlap_cnt, which under-counted high-frequency clients.
 *
 *   2. Quota calculation: added overuse feedback.  The old EWMA only
 *      tracked burst length; now overuse shrinks the next quota, and
 *      under-use grows it, keeping actual GPU time close to the
 *      allocated fraction.
 *
 *   3. Scheduling loop: removed the blocking sleep-until-quota-expires
 *      pattern.  The old loop granted a token then slept for the full
 *      quota duration, starving other clients.  Now the daemon
 *      immediately loops back to select the next candidate, relying on
 *      clients to re-enqueue themselves when they need a new token.
 */

 #include <iostream>
 #include <fstream>
 #include "scheduler.h"

 #include <arpa/inet.h>
 #include <errno.h>
 #include <execinfo.h>
 #include <getopt.h>
 #include <linux/limits.h>
 #include <netinet/in.h>
 #include <pthread.h>
 #include <signal.h>
 #include <sys/inotify.h>
 #include <sys/socket.h>
 #include <sys/time.h>
 #include <sys/types.h>
 #include <unistd.h>

 #include <algorithm>
 #include <chrono>
 #include <climits>
 #include <cmath>
 #include <cstdio>
 #include <cstdlib>
 #include <cstring>

 #include <limits>
 #include <list>
 #include <map>
 #include <string>
 #include <thread>
 #include <typeinfo>
 #include <vector>

 #include "debug.h"
 #include "util.h"
 #ifdef RANDOM_QUOTA
 #include <random>
 #endif

 using std::string;
 using std::chrono::duration_cast;
 using std::chrono::microseconds;
 using std::chrono::steady_clock;

 // signal handler
 void sig_handler(int);
 #ifdef _DEBUG
 void dump_history(int);
 #endif

 struct timespec get_timespec_after(double ms) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   double sec = ms / 1e3;
   ts.tv_sec += floor(sec);
   ts.tv_nsec += (sec - floor(sec)) * 1e9;
   ts.tv_sec += ts.tv_nsec / 1000000000;
   ts.tv_nsec %= 1000000000;
   return ts;
 }

 // all in milliseconds
 double QUOTA = 250.0;
 double MIN_QUOTA = 100.0;
 double WINDOW_SIZE = 10000.0;
 int verbosity = 0;
 char* log_name = "/xpushare/log/xhook-scheduler.log";
 #define EVENT_SIZE sizeof(struct inotify_event)
 #define BUF_LEN (1024 * (EVENT_SIZE + 16))
 auto PROGRESS_START = steady_clock::now();
 char limit_file_name[PATH_MAX] = "resource-config.txt";
 char limit_file_dir[PATH_MAX] = ".";

 std::list<History> history_list;
 #ifdef _DEBUG
 std::list<History> full_history;
 #endif

 inline double ms_since_start() {
   return duration_cast<microseconds>(steady_clock::now() - PROGRESS_START).count() / 1e3;
 }

 // map container name to object
 std::map<string, ClientInfo *> client_info_map;

 std::list<candidate_t> candidates;
 pthread_mutex_t candidate_mutex = PTHREAD_MUTEX_INITIALIZER;
 pthread_cond_t candidate_cond;  // initialized with CLOCK_MONOTONIC in main()

 // NOTE: candidate_mutex protects ALL shared mutable state:
 //   - candidates list
 //   - history_list
 //   - client_info_map (also written by monitor_file → read_resource_config)
 // Using a single mutex avoids ABBA deadlock between config_mutex and
 // candidate_mutex that existed in the previous version.

 // -----------------------------------------------------------------------
 // ClientInfo — v2: overuse-aware quota calculation
 // -----------------------------------------------------------------------

 ClientInfo::ClientInfo(double baseq, double minq, double maxq, double minf, double maxf)
     : BASE_QUOTA(baseq), MIN_QUOTA(minq), MAX_QUOTA(maxq), MIN_FRAC(minf), MAX_FRAC(maxf) {
   quota_ = BASE_QUOTA;
   last_granted_quota_ = BASE_QUOTA;
   latest_overuse_ = 0.0;
   latest_actual_usage_ = 0.0;
   burst_ = 0.0;
   overuse_ema_ = 0.0;
 }

 ClientInfo::~ClientInfo() {}

 void ClientInfo::set_burst(double estimated_burst) { burst_ = estimated_burst; }

 void ClientInfo::update_return_time(double overuse) {
   double now = ms_since_start();
   for (auto it = history_list.rbegin(); it != history_list.rend(); it++) {
     if (it->name == this->name) {
       it->end = std::min(now, it->end + overuse);
       latest_actual_usage_ = it->end - it->start;
       break;
     }
   }
   latest_overuse_ = overuse;

   // Update overuse EMA: ratio of overuse to the quota that was actually
   // granted for this token (not the current quota_ which may have been
   // updated by get_quota() already).
   if (last_granted_quota_ > 1e-9) {
     double overuse_ratio = overuse / last_granted_quota_;
     const double alpha = 0.3;
     overuse_ema_ = alpha * overuse_ratio + (1.0 - alpha) * overuse_ema_;
   }

 #ifdef _DEBUG
   for (auto it = full_history.rbegin(); it != full_history.rend(); it++) {
     if (it->name == this->name) {
       it->end = std::min(now, it->end + overuse);
       break;
     }
   }
 #endif
 }

 void ClientInfo::Record(double quota) {
   History hist;
   hist.name = this->name;
   hist.start = ms_since_start();
   hist.end = hist.start + quota;
   history_list.push_back(hist);
 #ifdef _DEBUG
   full_history.push_back(hist);
 #endif
 }

 double ClientInfo::get_min_fraction() { return MIN_FRAC; }
 double ClientInfo::get_max_fraction() { return MAX_FRAC; }

 /**
  * Overuse-aware self-adaptive quota — v2.
  *
  * The original only did:  quota = 0.5 * burst + 0.5 * quota  (EWMA on burst)
  * This ignored whether the client was actually over-using or under-using its
  * allocation.  A client with request=0.2 could get the same quota as one with
  * request=0.8 if they had similar burst patterns.
  *
  * New approach:
  *   1. Start from burst-based EWMA (same as before).
  *   2. Apply overuse correction: if the client consistently over-uses
  *      (overuse_ema_ > 0), shrink quota; if under-using, grow slightly.
  *   3. Clamp to [MIN_QUOTA, MAX_QUOTA].
  *
  * MAX_QUOTA is set to  max_fraction * WINDOW_SIZE  at construction time,
  * which is the absolute ceiling for this client's time-slice.
  */
 double ClientInfo::get_quota() {
   const double UPDATE_RATE = 0.5;

   if (burst_ < 1e-9) {
     quota_ = BASE_QUOTA;
     DEBUG(log_name, __FILE__, (long)__LINE__,
           "%s: no burst data, fallback quota=%.3fms", name.c_str(), quota_);
   } else {
     // Step 1: burst-based EWMA
     double burst_quota = burst_ * UPDATE_RATE + quota_ * (1.0 - UPDATE_RATE);

     // Step 2: overuse correction
     // overuse_ema_ > 0 means client consistently uses more than granted.
     // Shrink quota to bring actual usage closer to the target fraction.
     // overuse_ema_ < 0 means client returns early; grow slightly.
     double correction = 1.0 - 0.5 * overuse_ema_;
     // Clamp correction to [0.5, 1.3] to avoid extreme swings.
     correction = std::max(0.5, std::min(1.3, correction));
     quota_ = burst_quota * correction;

     // Step 3: clamp
     quota_ = std::max(quota_, MIN_QUOTA);
     quota_ = std::min(quota_, MAX_QUOTA);

     DEBUG(log_name, __FILE__, (long)__LINE__,
           "%s: burst=%.3f, overuse_ema=%.3f, correction=%.3f, quota=%.3fms",
           name.c_str(), burst_, overuse_ema_, correction, quota_);
   }
   last_granted_quota_ = quota_;
   return quota_;
 }

 void read_resource_config() {
   std::ifstream fin;
   ClientInfo *client_inf;
   char client_name[HOST_NAME_MAX], full_path[PATH_MAX];
   size_t gpu_memory_size;
   double gpu_min_fraction, gpu_max_fraction;
   int container_num;

   bzero(full_path, PATH_MAX);
   strncpy(full_path, limit_file_dir, PATH_MAX);
   if (limit_file_dir[strlen(limit_file_dir) - 1] != '/') full_path[strlen(limit_file_dir)] = '/';
   strncat(full_path, limit_file_name, PATH_MAX - strlen(full_path));

   fin.open(full_path, std::ios::in);
   if (!fin.is_open()) {
     ERROR(log_name, __FILE__, (long)__LINE__, "failed to open file %s: %s", full_path, strerror(errno));
     exit(1);
   }
   fin >> container_num;
   INFO(log_name, __FILE__, (long)__LINE__, "There are %d clients in the system...", container_num);

   pthread_mutex_lock(&candidate_mutex);
   for (int i = 0; i < container_num; i++) {
     fin >> client_name >> gpu_min_fraction >> gpu_max_fraction >> gpu_memory_size;
     client_inf = new ClientInfo(QUOTA, MIN_QUOTA, gpu_max_fraction * WINDOW_SIZE, gpu_min_fraction,
                                 gpu_max_fraction);
     client_inf->name = client_name;
     client_inf->gpu_mem_limit = gpu_memory_size;
     if (client_info_map.find(client_name) != client_info_map.end())
       delete client_info_map[client_name];
     client_info_map[client_name] = client_inf;
     INFO(log_name, __FILE__, (long)__LINE__, "%s request: %.2f, limit: %.2f, memory limit: %lu bytes",
          client_name, gpu_min_fraction, gpu_max_fraction, gpu_memory_size);
   }
   pthread_mutex_unlock(&candidate_mutex);
   fin.close();
 }

 void monitor_file(const char *path, const char *filename) {
   INFO(log_name, __FILE__, (long)__LINE__, "Monitor thread created.");
   int fd, wd;
   fd = inotify_init();
   if (fd < 0) ERROR(log_name, __FILE__, (long)__LINE__, "Failed to initialize inotify");
   wd = inotify_add_watch(fd, path, IN_CLOSE_WRITE);
   if (wd == -1)
     ERROR(log_name, __FILE__, (long)__LINE__, "Failed to add watch to '%s'.", path);
   else
     INFO(log_name, __FILE__, (long)__LINE__, "Watching '%s'.", path);

   while (1) {
     int i = 0;
     char buffer[BUF_LEN];
     bzero(buffer, BUF_LEN);
     int length = read(fd, buffer, BUF_LEN);
     if (length < 0) ERROR(log_name, __FILE__, (long)__LINE__, "Read error");
     while (i < length) {
       struct inotify_event *event = (struct inotify_event *)&buffer[i];
       if (event->len) {
         if (event->mask & IN_CLOSE_WRITE) {
           INFO(log_name, __FILE__, (long)__LINE__, "File %s modified.", (const char *)event->name);
           if (strcmp((const char *)event->name, filename) == 0) {
             INFO(log_name, __FILE__, (long)__LINE__, "Update containers' settings...");
             read_resource_config();
           }
         }
       }
       i += EVENT_SIZE + event->len;
     }
   }
   inotify_rm_watch(fd, wd);
   close(fd);
 }

 /**
  * Select the next candidate to receive a GPU time-slice token.
  *
  * v2 rewrite — usage accounting fix:
  *
  * The original code used an overlap-splitting algorithm that divided
  * overlapping history intervals by the overlap count.  This meant that
  * if client A and client B both had history in the same time range,
  * each was credited with only half the time — even if client A actually
  * ran for the full duration and B only overlapped briefly.  This
  * systematically under-counted high-frequency clients.
  *
  * The new approach simply sums each client's own history intervals
  * (clipped to the current window), which is both simpler and correct.
  * Overlapping intervals from *different* clients don't affect each
  * other's accounting — they represent concurrent GPU usage (which is
  * exactly what time-slicing allows during the granted quota).
  */
 candidate_t select_candidate() {
   while (true) {
     double now = ms_since_start();
     double window_start = now - WINDOW_SIZE;
     double window_size = WINDOW_SIZE;
     if (window_start < 0) {
       window_size = now;
       window_start = 0;
     }

     // Prune history entries that are entirely before the window.
     history_list.remove_if([=](const History &h) { return h.end < window_start; });

     // --- Per-client usage: sum of clipped intervals ---
     std::map<string, double> usage;
     for (auto &h : history_list) {
       double start = std::max(h.start, window_start);
       double end   = std::min(h.end, now);
       if (end > start) {
         usage[h.name] += (end - start);
       }
     }

     // Quick exit: if the front candidate has zero usage and is known, grant immediately.
     if (!candidates.empty()) {
       const string &front_name = candidates.front().name;
       if (usage.find(front_name) == usage.end() &&
           client_info_map.find(front_name) != client_info_map.end()) {
         candidate_t selected = candidates.front();
         candidates.pop_front();
         return selected;
       }
     }

     // Build valid candidate list.
     std::vector<valid_candidate_t> valid_candidates;
     for (auto it = candidates.begin(); it != candidates.end(); ) {
       string name = it->name;
       if (client_info_map.find(name) == client_info_map.end()) {
         // Client was removed by a config reload — drop this stale candidate.
         it = candidates.erase(it);
         continue;
       }

       double limit   = client_info_map[name]->get_max_fraction() * window_size;
       double require = client_info_map[name]->get_min_fraction() * window_size;
       double used    = usage[name];
       double missing    = require - used;
       double remaining  = limit - used;

       if (remaining > 0) {
         double deficit_ratio = (require > 1e-9) ? (missing / require) : 0.0;
         valid_candidates.push_back({missing, remaining, used, it->arrived_time,
                                     deficit_ratio, it});
       }
       ++it;
     }

     if (valid_candidates.empty()) {
       // All candidates have reached their limit.  Sleep until the oldest
       // history entry exits the window, or a new candidate arrives.
       if (!history_list.empty()) {
         double sleep_until = history_list.front().end - window_start;
         if (sleep_until < 1.0) sleep_until = 1.0;  // at least 1ms
         auto ts = get_timespec_after(sleep_until);
         DEBUG(log_name, __FILE__, (long)__LINE__,
               "all at limit, sleep %.1fms", sleep_until);
         pthread_cond_timedwait(&candidate_cond, &candidate_mutex, &ts);
       } else {
         // No history at all — just wait for a signal.
         struct timespec ts = get_timespec_after(100.0);
         pthread_cond_timedwait(&candidate_cond, &candidate_mutex, &ts);
       }
       continue;
     }

     std::sort(valid_candidates.begin(), valid_candidates.end(), schd_priority);

     auto selected_iter = valid_candidates.begin()->iter;
     candidate_t result = *selected_iter;
     candidates.erase(selected_iter);
     return result;
   }
 }

 void handle_message(int client_sock, char *message) {
   reqid_t req_id;
   comm_request_t req;
   size_t hostname_len, offset = 0;
   char sbuf[RSP_MSG_LEN];
   char *attached, *client_name;
   ClientInfo *client_inf;
   attached = parse_request(message, &client_name, &hostname_len, &req_id, &req);

   // All branches need client_info_map access, so take candidate_mutex once
   // at the top.  This also protects history_list (for REQ_QUOTA) and avoids
   // the ABBA deadlock that existed when config_mutex was separate.
   pthread_mutex_lock(&candidate_mutex);

   auto it = client_info_map.find(string(client_name));
   if (it == client_info_map.end()) {
     pthread_mutex_unlock(&candidate_mutex);
     WARNING(log_name, __FILE__, (long)__LINE__,
             "Unknown client \"%s\". Ignore this request.", client_name);
     return;
   }
   client_inf = it->second;
   bzero(sbuf, RSP_MSG_LEN);
   int rc, MAX_RETRY = 5;

   if (req == REQ_QUOTA) {
     double overuse, burst;
     overuse = get_msg_data<double>(attached, offset);
     burst   = get_msg_data<double>(attached, offset);

     client_inf->update_return_time(overuse);
     client_inf->set_burst(burst);
     candidates.push_back({client_sock, string(client_name), req_id, ms_since_start()});
     pthread_cond_signal(&candidate_cond);
     pthread_mutex_unlock(&candidate_mutex);

     DEBUG(log_name, __FILE__, (long)__LINE__,
           "enqueued REQ_QUOTA, client=%s, overuse=%.3f, burst=%.3f",
           client_name, overuse, burst);
     return;

   } else if (req == REQ_MEM_LIMIT) {
     prepare_response(sbuf, REQ_MEM_LIMIT, req_id, (size_t)0, client_inf->gpu_mem_limit);
     pthread_mutex_unlock(&candidate_mutex);
     rc = multiple_attempt(
         [&]() -> int {
           if (send(client_sock, sbuf, RSP_MSG_LEN, 0) == -1) return -1;
           return 0;
         },
         MAX_RETRY, 3);

   } else if (req == REQ_MEM_UPDATE) {
     prepare_response(sbuf, REQ_MEM_UPDATE, req_id, 1);
     pthread_mutex_unlock(&candidate_mutex);
     rc = multiple_attempt(
         [&]() -> int {
           if (send(client_sock, sbuf, RSP_MSG_LEN, 0) == -1) return -1;
           return 0;
         },
         MAX_RETRY, 3);

   } else {
     pthread_mutex_unlock(&candidate_mutex);
     WARNING(log_name, __FILE__, (long)__LINE__,
             "\"%s\" sent an unknown request.", client_name);
   }
 }

 /**
  * Schedule daemon — v2 rewrite (Bug 5 fix).
  *
  * Original problem:
  *   After granting a token, the old daemon slept for the full quota duration
  *   (pthread_cond_timedwait until quota expires).  During this sleep, NO
  *   other client could be scheduled, even if the GPU was idle.  This meant:
  *   - A client with request=0.2 could monopolize the GPU for its entire
  *     quota period while a client with request=0.8 waited in the queue.
  *   - The effective scheduling was serial: one token at a time, regardless
  *     of how many clients were waiting.
  *
  * Fix:
  *   The daemon now immediately loops back to select_candidate() after
  *   sending a token.  The client-side hook library is responsible for
  *   re-enqueuing itself (via REQ_QUOTA) when it needs a new token.
  *   select_candidate() already handles the case where all clients are at
  *   their limit by sleeping until the window shifts.
  *
  *   This allows true concurrent scheduling: if client A is using its token,
  *   client B can immediately get its own token without waiting for A to
  *   finish.  The usage accounting in select_candidate() ensures that no
  *   client exceeds its limit within the time window.
  */
 void *schedule_daemon_func(void *) {
 #ifdef RANDOM_QUOTA
   std::random_device rd;
   std::default_random_engine gen(rd());
   std::uniform_real_distribution<double> dis(0.4, 1.0);
 #endif
   double quota;

   while (1) {
     pthread_mutex_lock(&candidate_mutex);
     if (candidates.size() != 0) {
       candidate_t selected = select_candidate();
       DEBUG(log_name, __FILE__, (long)__LINE__,
             "select %s, wait=%.3fms",
             selected.name.c_str(),
             ms_since_start() - selected.arrived_time);

       // candidate_mutex is already held — it protects client_info_map too.
       quota = client_info_map[selected.name]->get_quota();
 #ifdef RANDOM_QUOTA
       quota *= dis(gen);
 #endif
       client_info_map[selected.name]->Record(quota);

       INFO(log_name, __FILE__, (long)__LINE__,
            "grant quota=%.3f to %s, req_id=%ld",
            quota, selected.name.c_str(), (long)selected.req_id);

       pthread_mutex_unlock(&candidate_mutex);

       // Send quota to the selected client.
       // Use short retry (1 attempt, no sleep) to avoid blocking the daemon.
       // If the client disconnected, the token is simply lost — the client
       // won't re-enqueue, so no harm done.
       char sbuf[RSP_MSG_LEN];
       bzero(sbuf, RSP_MSG_LEN);
       prepare_response(sbuf, REQ_QUOTA, selected.req_id, quota);

       if (send(selected.socket, sbuf, RSP_MSG_LEN, 0) == -1) {
         WARNING(log_name, __FILE__, (long)__LINE__,
                 "%s send failed: %s, token dropped",
                 selected.name.c_str(), strerror(errno));
       }

       // v2: Do NOT sleep here.  Immediately loop back to check for more
       // candidates.  The client will re-enqueue when it needs a new token.
       // This is the key fix for proportional time-slice control.

     } else {
       // No candidates — wait for incoming requests.
       DEBUG(log_name, __FILE__, (long)__LINE__, "no candidates, waiting");
       pthread_cond_wait(&candidate_cond, &candidate_mutex);
       pthread_mutex_unlock(&candidate_mutex);
     }
   }
 }

 void *pod_client_func(void *args) {
   int pod_sockfd = *((int *)args);
   char *rbuf = new char[REQ_MSG_LEN];
   ssize_t recv_rc;
   bzero(rbuf, REQ_MSG_LEN);

   while ((recv_rc = recv(pod_sockfd, rbuf, REQ_MSG_LEN, 0)) > 0) {
     DEBUG(log_name, __FILE__, (long)__LINE__, "pod_client_func recv -> handle message");
     handle_message(pod_sockfd, rbuf);
   }
   DEBUG(log_name, __FILE__, (long)__LINE__,
         "Connection closed by Pod manager. recv() returns %ld.", recv_rc);
   close(pod_sockfd);
   delete (int *)args;
   delete[] rbuf;
   pthread_exit(NULL);
 }

 int main(int argc, char *argv[]) {
   uint16_t schd_port = 50051;
   const char *optstring = "P:q:m:w:f:p:v:h";
   struct option opts[] = {{"port", required_argument, nullptr, 'P'},
                           {"quota", required_argument, nullptr, 'q'},
                           {"min_quota", required_argument, nullptr, 'm'},
                           {"window", required_argument, nullptr, 'w'},
                           {"limit_file", required_argument, nullptr, 'f'},
                           {"limit_file_dir", required_argument, nullptr, 'p'},
                           {"verbose", required_argument, nullptr, 'v'},
                           {"help", no_argument, nullptr, 'h'},
                           {nullptr, 0, nullptr, 0}};
   int opt;
   while ((opt = getopt_long(argc, argv, optstring, opts, NULL)) != -1) {
     switch (opt) {
       case 'P': schd_port = strtoul(optarg, nullptr, 10); break;
       case 'q': QUOTA = atof(optarg); break;
       case 'm': MIN_QUOTA = atof(optarg); break;
       case 'w': WINDOW_SIZE = atof(optarg); break;
       case 'f': strncpy(limit_file_name, optarg, PATH_MAX - 1); break;
       case 'p': strncpy(limit_file_dir, optarg, PATH_MAX - 1); break;
       case 'v': verbosity = atoi(optarg); break;
       case 'h':
         printf("usage: %s [options]\n", argv[0]);
         puts("Options:");
         puts("    -P [PORT], --port [PORT]");
         puts("    -q [QUOTA], --quota [QUOTA]");
         puts("    -m [MIN_QUOTA], --min_quota [MIN_QUOTA]");
         puts("    -w [WINDOW_SIZE], --window [WINDOW_SIZE]");
         puts("    -f [LIMIT_FILE], --limit_file [LIMIT_FILE]");
         puts("    -p [LIMIT_FILE_DIR], --limit_file_dir [LIMIT_FILE_DIR]");
         puts("    -v [LEVEL], --verbose [LEVEL]");
         puts("    -h, --help");
         return 0;
       default: break;
     }
   }

   if (verbosity > 0) {
     printf("XPUShare xhook-schd settings:\n");
     printf("    %-20s %.3f ms\n", "default quota:", QUOTA);
     printf("    %-20s %.3f ms\n", "minimum quota:", MIN_QUOTA);
     printf("    %-20s %.3f ms\n", "time window:", WINDOW_SIZE);
   }

   signal(SIGSEGV, sig_handler);
 #ifdef _DEBUG
   if (verbosity > 0) signal(SIGINT, dump_history);
 #endif

   read_resource_config();

   int rc;
   int sockfd = 0;
   int forClientSockfd = 0;
   struct sockaddr_in clientInfo;
   int addrlen = sizeof(clientInfo);

   std::thread t1(monitor_file, std::ref(limit_file_dir), std::ref(limit_file_name));
   t1.detach();

   sockfd = socket(AF_INET, SOCK_STREAM, 0);
   if (sockfd == -1) {
     ERROR(log_name, __FILE__, (long)__LINE__, "Fail to create a socket!");
     exit(-1);
   }

   struct sockaddr_in serverInfo;
   bzero(&serverInfo, sizeof(serverInfo));
   serverInfo.sin_family = PF_INET;
   serverInfo.sin_addr.s_addr = INADDR_ANY;
   serverInfo.sin_port = htons(schd_port);
   if (bind(sockfd, (struct sockaddr *)&serverInfo, sizeof(serverInfo)) < 0) {
     ERROR(log_name, __FILE__, (long)__LINE__, "cannot bind port");
     exit(-1);
   }
   listen(sockfd, SOMAXCONN);

   pthread_t tid;
   pthread_condattr_t attr_monotonic_clock;
   pthread_condattr_init(&attr_monotonic_clock);
   pthread_condattr_setclock(&attr_monotonic_clock, CLOCK_MONOTONIC);
   pthread_cond_init(&candidate_cond, &attr_monotonic_clock);

   rc = pthread_create(&tid, NULL, schedule_daemon_func, NULL);
   if (rc != 0) {
     ERROR(log_name, __FILE__, (long)__LINE__, "Return code from pthread_create(): %d", rc);
     exit(rc);
   }
   pthread_detach(tid);
   INFO(log_name, __FILE__, (long)__LINE__, "Waiting for incoming connection");

   while ((forClientSockfd = accept(sockfd, (struct sockaddr *)&clientInfo, (socklen_t *)&addrlen))) {
     INFO(log_name, __FILE__, (long)__LINE__, "Received an incoming connection.");
     pthread_t tid;
     int *pod_sockfd = new int;
     *pod_sockfd = forClientSockfd;
     pthread_create(&tid, NULL, pod_client_func, pod_sockfd);
     pthread_detach(tid);
   }
   if (forClientSockfd < 0) {
     ERROR(log_name, __FILE__, (long)__LINE__, "Accept failed");
     return 1;
   }
   return 0;
 }

 void sig_handler(int sig) {
   void *arr[10];
   size_t s;
   s = backtrace(arr, 10);
   ERROR(log_name, __FILE__, (long)__LINE__, "Received signal %d", sig);
   backtrace_symbols_fd(arr, s, STDERR_FILENO);
   exit(sig);
 }

 #ifdef _DEBUG
 void dump_history(int sig) {
   char filename[20];
   sprintf(filename, "%ld.json", time(NULL));
   FILE *f = fopen(filename, "w");
   fputs("[\n", f);
   for (auto it = full_history.begin(); it != full_history.end(); it++) {
     fprintf(f, "\t{\"container\": \"%s\", \"start\": %.3lf, \"end\" : %.3lf}", it->name.c_str(),
             it->start / 1000.0, it->end / 1000.0);
     if (std::next(it) == full_history.end())
       fprintf(f, "\n");
     else
       fprintf(f, ",\n");
   }
   fputs("]\n", f);
   fclose(f);
   INFO(log_name, __FILE__, (long)__LINE__, "history dumped to %s", filename);
   exit(0);
 }
 #endif

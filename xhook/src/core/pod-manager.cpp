/**
 * Copyright 2020 Hung-Hsin Chen, LSA Lab, National Tsing Hua University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * From Kubernetes concepts: A Pod is the basic execution unit of a Kubernetes application–the
 * smallest and simplest unit in the Kubernetes object model that you create or deploy. A Pod
 * represents processes running on your cluster.
 *
 * This manager will run like a daemon in each Pod. User program will interact with this manager
 * when they call certain CUDA-related functions.
 */

 #include <arpa/inet.h>
 #include <execinfo.h>
 #include <pthread.h>
 #include <signal.h>
 #include <sys/socket.h>
 #include <unistd.h>
 // #include <fcntl.h> 
 #include <string>
 #include <cassert>
 #include <cerrno>
 #include <chrono>
 #include <climits>
 #include <cstdio>
 #include <cstdlib>
 #include <cstring>
 #include <functional>
 #include <map>
 #include <queue>
 #include <vector>
 #include <iostream>
 #include <fstream>
 #include "comm.h"
 #include "debug.h"
 #include "util.h"
 std::ofstream myfile ("/tmp/pod.txt");
 using std::chrono::duration_cast;
 using std::chrono::microseconds;
 using std::chrono::steady_clock;
 using std::chrono::time_point;
 using std::string;
 // connection information, below are default values
 // can be changed by environment vairables
 char SCHEDULER_IP[20] = "127.0.0.1";
 uint16_t SCHEDULER_PORT = 50051;
 uint16_t POD_SERVER_PORT = 50052;
 char* log_name = "/xpushare/log/xhook-pmgr.log";
 void sig_handler(int);
 
 // thread interact with scheduler
 void *scheduler_thread_send_func(void *sockfd);
 void *scheduler_thread_recv_func(void *sockfd);
 // service thread for each hook library
 void *hook_thread_func(void *sockfd);
 
 /* communication between scheduler thread and hook threads */
 enum actions {
   KERNEL_LAUNCH,
 };
 struct request {
   reqid_t req_id;
   char *data;
 };
 std::queue<request> request_queue;
 uint32_t req_cnt = 0;
 pthread_mutex_t req_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
 pthread_cond_t req_queue_cond = PTHREAD_COND_INITIALIZER;
 
 struct response {
   void *data;
 };
 std::map<reqid_t, response> response_map;
 pthread_mutex_t rsp_map_mutex = PTHREAD_MUTEX_INITIALIZER;
 pthread_cond_t rsp_map_cond = PTHREAD_COND_INITIALIZER;
 
 /* global variables to store memory limit */
 size_t gpu_mem_limit = 0, gpu_mem_used = 0;
 std::map<int, size_t> allocation_map;  // memory usage of each connection
 pthread_mutex_t mem_info_mutex = PTHREAD_MUTEX_INITIALIZER;
 
/* computation utilization */
typedef time_point<steady_clock> quota_tp;
std::map<int, double> client_burst_map;
pthread_mutex_t client_stat_mutex = PTHREAD_MUTEX_INITIALIZER;

// All quota-related state is protected by quota_mutex.
// Only one thread at a time may request a new quota from the scheduler;
// other threads wait on quota_cond until the new quota arrives.
pthread_mutex_t quota_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  quota_cond  = PTHREAD_COND_INITIALIZER;
double pod_overuse_ms = 0.0;
double pod_quota = 0.0;
quota_tp quota_updated_tp;
bool   quota_updating = false;  // true while a thread is fetching new quota
 
 
 /* communication with scheduler */
 size_t pod_name_len;
 char pod_name[HOST_NAME_MAX];
 
 // retrieve memory limit information from scheduler
 int retrieve_mem_info(int sockfd, const int MAX_RETRY, const long RETRY_TIMEOUT) {
   int rc;
   char sbuf[REQ_MSG_LEN], rbuf[RSP_MSG_LEN], *attached;
   size_t pos = 0;
 
   // set socket timeout option
   struct timeval tv = {RETRY_TIMEOUT, 0};
   setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
 
   bzero(sbuf, REQ_MSG_LEN);
   prepare_request(sbuf, REQ_MEM_LIMIT);
 
   rc = multiple_attempt(
       [&]() -> int {
         if (send(sockfd, sbuf, REQ_MSG_LEN, 0) == -1) return -1;
         if (recv(sockfd, rbuf, RSP_MSG_LEN, 0) == -1) return -1;
         return 0;
       },
       MAX_RETRY, 0);
 
   // disable timeout option
   tv.tv_sec = 0;
   setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
 
   if (rc != 0) return rc;  // failed to get memory info from scheduler
 
   // parse received data and get memory limit
   attached = parse_response(rbuf, nullptr);
   gpu_mem_used = get_msg_data<size_t>(attached, pos);  // should be 0
   gpu_mem_limit = get_msg_data<size_t>(attached, pos);
   assert(gpu_mem_used == (size_t)0);
   INFO(log_name, __FILE__, (long)__LINE__, "GPU memory limit: %lu bytes.", gpu_mem_limit);
   return 0;
 }
 
 int main(int argc, char *argv[]) {
   const int NET_OP_MAX_ATTEMPT = 5;  // maximum time retrying failed network operations
   const int NET_OP_RETRY_INTV = 10;  // seconds between two retries
   int rc;
 
   // for debugging
   signal(SIGSEGV, sig_handler);
 
   // use host name as Pod name
   char *name = getenv("POD_NAME");
   if (name != NULL) {
     myfile<<"pod name: "<<name<<std::endl;
     strcpy(pod_name, name);
   } else {
     gethostname(pod_name, HOST_NAME_MAX);
     myfile<<"pod name (hostname): "<<pod_name<<std::endl;
   }
   pod_name_len = strlen(pod_name);
 
   /* get connection information from environment variable */
   // Pod server
   char *pod_server_port_str = getenv("POD_MANAGER_PORT");
   if (pod_server_port_str != NULL) {
     POD_SERVER_PORT = atoi(pod_server_port_str);
   }
   INFO(log_name, __FILE__, (long)__LINE__, "Pod server port = %u.", POD_SERVER_PORT);
 
   // scheduler
   char *scheduler_ip_envstr = getenv("SCHEDULER_IP");
   if (scheduler_ip_envstr != NULL) {
     strcpy(SCHEDULER_IP, scheduler_ip_envstr);
   }
   char *scheduler_port_envstr = getenv("SCHEDULER_PORT");
   if (scheduler_port_envstr != NULL) {
     SCHEDULER_PORT = atoi(scheduler_port_envstr);
   }
   INFO(log_name, __FILE__, (long)__LINE__, "scheduler %s:%u", SCHEDULER_IP, SCHEDULER_PORT);
 
   /* establish connection with scheduler */
   // create socket
   int schd_sockfd = socket(PF_INET, SOCK_STREAM, 0);
   if (schd_sockfd == -1) {
     int err = errno;
     ERROR(log_name, __FILE__, (long)__LINE__, "failed to create socket: %s", strerror(err));
     exit(err);
   }
 
   // setup socket info
   struct sockaddr_in schd_info;
   bzero(&schd_info, sizeof(schd_info));
   schd_info.sin_family = AF_INET;
   schd_info.sin_addr.s_addr = inet_addr(SCHEDULER_IP);
   schd_info.sin_port = htons(SCHEDULER_PORT);
 
   // connect to scheduler
   rc = multiple_attempt(
       [&]() -> int {
         return connect(schd_sockfd, (struct sockaddr *)&schd_info, sizeof(schd_info));
       },
       NET_OP_MAX_ATTEMPT, NET_OP_RETRY_INTV);
   if (rc != 0) exit(rc);
 
   /* get memory limit for this pod */
   //rc = retrieve_mem_info(schd_sockfd, NET_OP_MAX_ATTEMPT, NET_OP_RETRY_INTV);
   //if (rc != 0) exit(rc);
   // 新代码：
   INFO(log_name, __FILE__, (long)__LINE__, "Attempting to retrieve memory info from scheduler...");
   rc = retrieve_mem_info(schd_sockfd, NET_OP_MAX_ATTEMPT, NET_OP_RETRY_INTV);
   
   // 检查是否因为竞态条件而失败 (rc != 0)
   while (rc != 0) {
       // 记录失败，并等待 1 秒钟
      WARNING(log_name, __FILE__, (long)__LINE__, "Failed to retrieve memory info (rc=%d). Retrying in 1 second...", rc);
      sleep(1); // 等待 xhook-schd 更新其配置
       
       // 再次尝试获取 (使用更短的超时，因为 xhook-schd 应该已经准备好了)
       rc = retrieve_mem_info(schd_sockfd, 2, 1);
   }
   // 循环结束，说明 retrieve_mem_info 成功了 (rc == 0)
   INFO(log_name, __FILE__, (long)__LINE__, "Successfully retrieved memory info.");
 
   // initialize quota receiving time
   quota_updated_tp = steady_clock::now();
 
   /* accept connections from hook libraries */
   // create accept socket
   int accept_sockfd = socket(PF_INET, SOCK_STREAM, 0);
   if (accept_sockfd == -1) {
     ERROR(log_name, __FILE__, (long)__LINE__, "accept_socket == -1");
     exit(-1);
   }
 
   // setup accept socket info
   struct sockaddr_in server_info;
   bzero(&server_info, sizeof(server_info));
   server_info.sin_family = AF_INET;
   server_info.sin_addr.s_addr = INADDR_ANY;
   server_info.sin_port = htons(POD_SERVER_PORT);
 
   rc = multiple_attempt(
       [&]() -> int {
         return bind(accept_sockfd, (struct sockaddr *)&server_info, sizeof(server_info));
       },
       NET_OP_MAX_ATTEMPT, NET_OP_RETRY_INTV);
   if (rc != 0) exit(rc);
   listen(accept_sockfd, SOMAXCONN);
 
   // start scheduler threads
   pthread_t schd_send_tid, schd_recv_tid;
   pthread_create(&schd_send_tid, NULL, scheduler_thread_send_func, &schd_sockfd);
   pthread_create(&schd_recv_tid, NULL, scheduler_thread_recv_func, &schd_sockfd);
   pthread_detach(schd_send_tid);
   pthread_detach(schd_recv_tid);
 
   int client_sockfd = 0;
   struct sockaddr_in client_info;
   int addr_len = sizeof(client_info);
 
   // wait for incoming connections
   while ((client_sockfd =
               accept(accept_sockfd, (struct sockaddr *)&client_info, (socklen_t *)&addr_len))) {
     if (client_sockfd == -1) {
       ERROR(log_name, __FILE__, (long)__LINE__, "accept() return -1");
       break;
     }
 
    // create allocation accounting entry
    pthread_mutex_lock(&mem_info_mutex);
    allocation_map.insert(std::make_pair(client_sockfd, 0));
    pthread_mutex_unlock(&mem_info_mutex);
 
     // create client statistics entries
     pthread_mutex_lock(&client_stat_mutex);
     client_burst_map.insert(std::make_pair(client_sockfd, 0.0));
     pthread_mutex_unlock(&client_stat_mutex);
 
     // create a thread for each client
     pthread_t tid;
     int *sockfd = new int;
     *sockfd = client_sockfd;
     pthread_create(&tid, NULL, hook_thread_func, (void *)sockfd);
     pthread_detach(tid);
   }
 
   return 0;
 }
 
 void sig_handler(int sig) {
   void *arr[10];
   size_t s = backtrace(arr, 10);
   ERROR(log_name, __FILE__, (long)__LINE__, "Received signal %d", sig);
   backtrace_symbols_fd(arr, s, STDERR_FILENO);
   exit(sig);
 }
 
// update GPU memory usage information
int hook_update_memory_usage(size_t mem_size, int allocate, int sockfd) {
  int ok = 1;  // meets memory limit
  pthread_mutex_lock(&mem_info_mutex);
  if (allocate) {
    if (gpu_mem_used + mem_size > gpu_mem_limit) {
      ok = 0;
    } else {
      gpu_mem_used += mem_size;
      allocation_map[sockfd] += mem_size;
    }
  } else {
    // Guard against unsigned underflow: if mem_size > current value,
    // clamp to 0 instead of wrapping to a huge number.
    if (mem_size > gpu_mem_used) {
      WARNING(log_name, __FILE__, (long)__LINE__,
              "gpu_mem_used underflow: used=%lu, freeing=%lu", gpu_mem_used, mem_size);
      gpu_mem_used = 0;
    } else {
      gpu_mem_used -= mem_size;
    }
    if (mem_size > allocation_map[sockfd]) {
      allocation_map[sockfd] = 0;
    } else {
      allocation_map[sockfd] -= mem_size;
    }
  }
  DEBUG(log_name, __FILE__, (long)__LINE__, "GPU memory usage = %lu bytes.", gpu_mem_used);
  
  pthread_mutex_unlock(&mem_info_mutex);
  return ok;
}
 
/**
 * Handle a kernel launch request from a hook library thread.
 *
 * v2 rewrite — simplified synchronization model.
 *
 * The original code used 5 separate mutexes (quota_state_mutex,
 * kernel_launch_count_mutex, sleeping_count_mutex, scheduler_recv_sync_mutex,
 * rsp_map_mutex) with a complex sleeping_count barrier to coordinate multiple
 * hook threads.  This had several bugs:
 *
 *   1. pod_overuse_ms, pod_quota, quota_updated_tp were read/written without
 *      any lock — data race between concurrent hook threads.
 *   2. The sleeping_count barrier (waiting for sleeping_count >= kernel_launch_count)
 *      could deadlock if a thread incremented kernel_launch_count but hadn't yet
 *      reached the quota_state check.
 *   3. gpu_mem_used subtraction could underflow (unsigned).
 *
 * New model: a single quota_mutex protects all quota-related state.
 * When quota expires, the first thread to detect it sets quota_updating=true,
 * releases quota_mutex, fetches a new quota from the scheduler, then
 * re-acquires quota_mutex, updates state, and broadcasts to wake waiters.
 * Other threads simply wait on quota_cond.
 */
double hook_kernel_launch(int sockfd, double overuse_ms, double burst, char* client_name) {
  // update per-client burst statistics (separate lock, no contention with quota)
  pthread_mutex_lock(&client_stat_mutex);
  client_burst_map[sockfd] = burst;
  pthread_mutex_unlock(&client_stat_mutex);

  pthread_mutex_lock(&quota_mutex);

  // merge overuse: keep the worst case across all clients in this Pod
  if (overuse_ms > pod_overuse_ms) {
    pod_overuse_ms = overuse_ms;
  }

  // wait if another thread is already fetching a new quota
  while (quota_updating) {
    DEBUG(log_name, __FILE__, (long)__LINE__,
          "%s waiting for quota update by another thread", client_name);
    pthread_cond_wait(&quota_cond, &quota_mutex);
  }

  // check if we need a new quota
  quota_tp now_tp = steady_clock::now();
  double elapsed_time = duration_cast<microseconds>(now_tp - quota_updated_tp).count() / 1e3;

  if (elapsed_time + burst > pod_quota) {
    // We are the thread that will fetch a new quota.
    quota_updating = true;

    // Snapshot values we need for the request, then release the lock
    // so other threads can still enter and wait (not block on mutex).
    double req_overuse = pod_overuse_ms;

    pthread_mutex_lock(&client_stat_mutex);
    double max_burst = 0.0;
    for (auto &x : client_burst_map) max_burst = std::max(x.second, max_burst);
    pthread_mutex_unlock(&client_stat_mutex);

    pthread_mutex_unlock(&quota_mutex);

    // --- Outside quota_mutex: do the blocking network I/O ---
    char *sbuf = new char[REQ_MSG_LEN];
    bzero(sbuf, REQ_MSG_LEN);
    reqid_t req_id;
    bool complete = false;
    size_t rpos = 0;

    // place request into request queue
    pthread_mutex_lock(&req_queue_mutex);
    req_id = prepare_request(sbuf, REQ_QUOTA, req_overuse, max_burst);
    request_queue.push({req_id, sbuf});
    pthread_cond_signal(&req_queue_cond);
    pthread_mutex_unlock(&req_queue_mutex);

    DEBUG(log_name, __FILE__, (long)__LINE__,
          "%s sent REQ_QUOTA to scheduler, req_id=%ld, overuse=%.3f, burst=%.3f",
          client_name, (long)req_id, req_overuse, max_burst);

    // wait for response
    while (!complete) {
      pthread_mutex_lock(&rsp_map_mutex);
      // Check before waiting — response may already be here.
      if (response_map.find(req_id) != response_map.end()) {
        double new_quota = get_msg_data<double>((char *)response_map[req_id].data, rpos);
        delete[] (char *)response_map[req_id].data;
        response_map.erase(req_id);
        pthread_mutex_unlock(&rsp_map_mutex);

        // Re-acquire quota_mutex to update shared state.
        pthread_mutex_lock(&quota_mutex);
        pod_quota = new_quota;
        quota_updated_tp = steady_clock::now();
        pod_overuse_ms = 0.0;
        elapsed_time = 0.0;
        quota_updating = false;
        pthread_cond_broadcast(&quota_cond);  // wake all waiting threads
        // quota_mutex stays held — we'll use elapsed_time below
        complete = true;

        INFO(log_name, __FILE__, (long)__LINE__,
             "%s new quota=%.3fms, req_id=%ld", client_name, new_quota, (long)req_id);
      } else {
        pthread_cond_wait(&rsp_map_cond, &rsp_map_mutex);
        pthread_mutex_unlock(&rsp_map_mutex);
      }
    }

    delete[] sbuf;
    // quota_mutex is held here

  }
  // else: quota is still valid, quota_mutex is held

  double remaining = pod_quota - elapsed_time;
  pthread_mutex_unlock(&quota_mutex);
  return remaining;
}
 
 // a thread interact with a hook library
 void *hook_thread_func(void *args) {
   DEBUG(log_name, __FILE__, (long)__LINE__, "hook thread started.");
   INFO(log_name, __FILE__, (long)__LINE__, "hook thread started.");
   int sockfd = *((int *)args);
   char rbuf[REQ_MSG_LEN], sbuf[RSP_MSG_LEN];
   char  *client_name = nullptr;

   
   //bzero(rbuf, REQ_MSG_LEN);
   ssize_t rc;
   int recv_zero_times = 0;
   while (recv_zero_times <= 5) {
     if((rc = recv(sockfd, rbuf, REQ_MSG_LEN, 0)) <= 0){
       recv_zero_times++;
       DEBUG(log_name, __FILE__, (long)__LINE__, "%s hook_thread_func recv - len <= 0, cnt %ld",
             client_name ? client_name : "(unknown)", recv_zero_times);
       continue;
     } 
     comm_request_t req;
     reqid_t rid;
     size_t pos = 0;  // attached data reading position
     size_t len = 0;  // length of sending data
     
     char *attached = parse_request(rbuf, &client_name, nullptr, &rid, &req);
 
     bzero(sbuf, RSP_MSG_LEN);
    if (req == REQ_MEM_LIMIT) {
      // send gpu_mem_used and gpu_mem_limit to hook library
      pthread_mutex_lock(&mem_info_mutex);
      size_t used_snap = gpu_mem_used;
      size_t limit_snap = gpu_mem_limit;
      pthread_mutex_unlock(&mem_info_mutex);
      len = prepare_response(sbuf, REQ_MEM_LIMIT, rid, used_snap, limit_snap);
      DEBUG(log_name, __FILE__, (long)__LINE__, "%s hook_thread_func recv - REQ_MEM_LIMIT, %ld", client_name, rid);
     } else if (req == REQ_MEM_UPDATE) {
       // update memory usage
       size_t mem_size = get_msg_data<size_t>(attached, pos);
       int allocate = get_msg_data<int>(attached, pos);
       int ok = hook_update_memory_usage(mem_size, allocate, sockfd);
       len = prepare_response(sbuf, REQ_MEM_UPDATE, rid, ok);
      DEBUG(log_name, __FILE__, (long)__LINE__, "%s hook_thread_func recv - REQ_MEM_UPDATE, %ld", client_name, rid);
    } else if (req == REQ_QUOTA) {
      // check if there is available quota
      double overuse_ms = get_msg_data<double>(attached, pos);
      double burst      = get_msg_data<double>(attached, pos);

      INFO(log_name, __FILE__, (long)__LINE__,
           "%s REQ_QUOTA: overuse=%.3fms burst=%.3fms rid=%ld",
           client_name, overuse_ms, burst, rid);

      // 这里会阻塞等待 scheduler 返回
      double quota_remain = hook_kernel_launch(sockfd, overuse_ms, burst, client_name);

      INFO(log_name, __FILE__, (long)__LINE__,
           "%s REQ_QUOTA done: quota_remain=%.3f rid=%ld",
           client_name, quota_remain, rid);

      // return remaining quota time
      len = prepare_response(sbuf, REQ_QUOTA, rid, quota_remain);
    }
     
     if (len > 0) {
       // have message to send
       if (send(sockfd, sbuf, RSP_MSG_LEN, 0) == -1) {
         ERROR(log_name, __FILE__, (long)__LINE__, "failed to send message to hook library!");
       }
       DEBUG(log_name, __FILE__, (long)__LINE__, "%s hook_thread_func send, %ld", client_name ,rid);
     }
   }
   
   INFO(log_name, __FILE__, (long)__LINE__, "%s connection closed by peer. recv() returns %ld.",
        client_name ? client_name : "(unknown)", rc);
   // since hook library close socket only when process terminated, we can use this as an indicator
   // of process termination, and recover memory usage
   pthread_mutex_lock(&mem_info_mutex);
   size_t to_free = allocation_map[sockfd];
   if (to_free > gpu_mem_used) {
     gpu_mem_used = 0;
   } else {
     gpu_mem_used -= to_free;
   }
   allocation_map.erase(sockfd);
   DEBUG(log_name, __FILE__, (long)__LINE__, "GPU memory usage = %lu bytes.", gpu_mem_used);
   
   pthread_mutex_unlock(&mem_info_mutex);
 
   pthread_mutex_lock(&client_stat_mutex);
   client_burst_map.erase(sockfd);
   pthread_mutex_unlock(&client_stat_mutex);
 
   close(sockfd);
   delete (int *)args;
   pthread_exit(NULL);
 }
 
// forward requests to scheduler
void *scheduler_thread_send_func(void *args) {
  DEBUG(log_name, __FILE__, (long)__LINE__, "scheduler_thread_send_func");
  int sockfd = *((int *)args);
  ssize_t send_rc;
  std::vector<request> batch;  // local buffer to drain queue under lock

  while (true) {
    pthread_mutex_lock(&req_queue_mutex);

    // Wait until there is something in the queue.
    while (request_queue.empty()) {
      pthread_cond_wait(&req_queue_cond, &req_queue_mutex);
    }

    // Drain all pending requests into a local batch, then release the lock
    // so hook threads can enqueue new requests while we do blocking I/O.
    while (!request_queue.empty()) {
      batch.push_back(request_queue.front());
      request_queue.pop();
    }
    pthread_mutex_unlock(&req_queue_mutex);

    // Send outside the lock.
    for (auto &req : batch) {
      if ((send_rc = send(sockfd, req.data, REQ_MSG_LEN, 0)) <= 0) {
        ERROR(log_name, __FILE__, (long)__LINE__, "failed to send request to scheduler! return code %ld.", send_rc);
      } else {
        DEBUG(log_name, __FILE__, (long)__LINE__, "send a kernel launch request, req_id: %ld", (long)req.req_id);
      }
    }
    batch.clear();
  }
  pthread_exit(NULL);
}
 
// receive response from scheduler and place responded data into response_map
void *scheduler_thread_recv_func(void *args) {
  int sockfd = *((int *)args);

  char buf[RSP_MSG_LEN], *attached;
  ssize_t rc;
  DEBUG(log_name, __FILE__, (long)__LINE__, "scheduler_thread_recv_func started");

  while ((rc = recv(sockfd, buf, RSP_MSG_LEN, 0)) > 0) {
    // process response
    reqid_t req_id;
    response rsp;

    attached = parse_response(buf, &req_id);
    rsp.data = new char[RSP_MSG_LEN - sizeof(reqid_t)];
    memcpy(rsp.data, attached, RSP_MSG_LEN - sizeof(reqid_t));

    INFO(log_name, __FILE__, (long)__LINE__,
         "scheduler_thread_recv_func: recv response, req_id=%ld", req_id);

    // put response data into response_map and notify hook threads
    pthread_mutex_lock(&rsp_map_mutex);
    response_map.insert(std::make_pair(req_id, rsp));
    pthread_cond_broadcast(&rsp_map_cond);  // wake ALL waiters so each can check its req_id
    pthread_mutex_unlock(&rsp_map_mutex);
  }

  WARNING(log_name, __FILE__, (long)__LINE__,
          "connection closed by scheduler. recv() returns %ld.", rc);
  close(sockfd);
  pthread_exit(NULL);
}
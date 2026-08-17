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
 * The library to intercept applications' CUDA-related function calls.
 *
 * This hook library will try connecting to scheduling system when first intercepted function being
 * called (with information specified in environment variables). After then, all CUDA kernel
 * launches and some GPU memory-related activity will be controlled by this hook library.
 */

 #include "hook.h"

 #include <arpa/inet.h>
 #include <cuda.h>
 #include <cuda_runtime.h>
 #include <dlfcn.h>
 #include <netinet/in.h>
 #include <pthread.h>
 #include <semaphore.h>
 #include <sys/socket.h>
 #include <sys/time.h>
 #include <sys/types.h>
 #include <unistd.h>
 
 
 #include <climits>
 #include <cmath>
 #include <cstdio>
 #include <cstdlib>
 #include <cstring>
 #include <strings.h>
 #include <fstream>
 #include <iostream>
 #include <map>
 #include <queue>
 #include <random>
 #include <sstream>
 
 #include "../../core/comm.h"
 #include "../../core/debug.h"
 #include "../../core/predictor.h"
 #include "../../core/util.h"
 
 
 CUresult CUDAAPI cuMemAlloc_hook( CUdeviceptr* dptr, size_t bytesize) {
   cuMemAlloc(dptr,bytesize); 
   printf("allocate %zu bytes.\n", bytesize);
 
   return CUDA_SUCCESS;
 
 }
 
 /*
 extern "C" {
 void *__libc_dlsym(void *map, const char *name);
 }
 extern "C" {
 void *__libc_dlopen_mode(const char *name, int mode);
 }
 */
 
 #define STRINGIFY(x) #x
 #define CUDA_SYMBOL_STRING(x) STRINGIFY(x)
 
 
 void *libdlHandle = NULL;
 void *libcudaHandle = NULL;
 void *libcudnnHandle = NULL;
 
 typedef void *(*fnDlsym)(void *, const char *);
 CUresult CUDAAPI cuGetProcAddress(const char *symbol, void **pfn, int cudaVersion, cuuint64_t flags);
 
 
 
 struct hookInfo {
   int debug_mode = 0;
   int coverage_mode = 0;
   void *preHooks[NUM_HOOK_SYMBOLS];
   void *postHooks[NUM_HOOK_SYMBOLS];
   int call_count[NUM_HOOK_SYMBOLS];
   void *func_actual[NUM_HOOK_SYMBOLS];
  
   hookInfo() {
     const char *envHookDebug;
     const char *envCoverage;
     const char *envProfiling;
 
     envHookDebug = getenv("CU_HOOK_DEBUG");
     if (envHookDebug && envHookDebug[0] == '1')
       debug_mode = 1;
     else
       debug_mode = 0;
 
     // Coverage is a low-intrusiveness profiling signal; keep it independent
     // from debug logs. CU_HOOK_DEBUG=1 implies coverage.
     envCoverage = getenv("XPUSHARE_DRIVER_COVERAGE");
     envProfiling = getenv("XPUSHARE_PROFILING");
     if ((envCoverage && envCoverage[0] == '1') || (envProfiling && envProfiling[0] == '1') ||
         debug_mode == 1) {
       coverage_mode = 1;
     } else {
       coverage_mode = 0;
     }
 
     memset(preHooks, 0, sizeof(preHooks));
     memset(postHooks, 0, sizeof(postHooks));
     memset(call_count, 0, sizeof(call_count));
     memset(func_actual, 0, sizeof(func_actual));
   }
 };
 
 static struct hookInfo hook_inf;
 const char* log_name = "/xpushare/log/hook.log";
 /*
 
 
 /* connection with Pod manager */
std::string scheduler_ip_file = "/xpushare/library/schedulerIP.txt";
std::string scheduler_port_file = "/xpushare/schedulerPort.txt";
 char pod_manager_ip[20] = "127.0.0.1";
 uint16_t pod_manager_port = 50052;                       // default value
 pthread_mutex_t comm_mutex = PTHREAD_MUTEX_INITIALIZER;  // one communication at a time
 const int NET_OP_MAX_ATTEMPT = 5;  // maximum time retrying failed network operations
 const int NET_OP_RETRY_INTV = 10;  // seconds between two retries
 
 /* GPU computation resource usage */
 double quota_time = 0;  // time quota from scheduler
 double overuse = 0;     // overuse time (ms)
 pthread_mutex_t request_time_mutex = PTHREAD_MUTEX_INITIALIZER;
 
 // predictors
 const double SCHD_OVERHEAD = 2.0;                   // ms
 Predictor burst_predictor("burst", SCHD_OVERHEAD);  // predicted burst may not be the full burst
 Predictor window_predictor("window");
 
 pthread_mutex_t expiration_status_mutex = PTHREAD_MUTEX_INITIALIZER;
 
 static pthread_once_t init_done = PTHREAD_ONCE_INIT;
 cudaEvent_t cuevent_start;      // the time receive new token
 struct timespec request_start;  // the time receive new token
 
 pthread_mutex_t overuse_trk_mutex = PTHREAD_MUTEX_INITIALIZER;
 pthread_cond_t overuse_trk_strt_cond = PTHREAD_COND_INITIALIZER;
 pthread_cond_t overuse_trk_cmpl_cond = PTHREAD_COND_INITIALIZER;
 pthread_cond_t overuse_trk_intr_cond;  // initialize it with CLOCK_MONOTONIC
 bool overuse_trk_cmpl;
 
 // 调试与强制请求开关（通过环境变量控制）
 static int g_force_req_quota = -1;
 static int g_extra_debug     = -1;
 
// A single libxhook.so.1 may interpose both runtime and driver APIs. When
// runtime shim enforcement is enabled, the driver hooks can be reached via the
// same call chain (e.g., cudaMalloc -> cuMemAlloc). Use a TLS flag to avoid
// double enforcement.
extern "C" __thread int g_xpushare_in_runtime_shim;
 
typedef enum XpushareEnforcementLayerEnum {
  XPUSHARE_LAYER_DRIVER = 0,
  XPUSHARE_LAYER_RUNTIME = 1,
  XPUSHARE_LAYER_NONE = 2,
  XPUSHARE_LAYER_PROFILE = 3,
} XpushareEnforcementLayer;
 
 static int g_enforcement_layer = -1;
 
static XpushareEnforcementLayer xpushare_enforcement_layer() {
  if (g_enforcement_layer >= 0) {
    return (XpushareEnforcementLayer)g_enforcement_layer;
  }

  const char *profiling = getenv("XPUSHARE_PROFILING");
  if (profiling && profiling[0] == '1') {
    g_enforcement_layer = (int)XPUSHARE_LAYER_PROFILE;
    return XPUSHARE_LAYER_PROFILE;
  }

  const char *e = getenv("XPUSHARE_ENFORCEMENT_LAYER");
  if (!e || !e[0]) {
    g_enforcement_layer = (int)XPUSHARE_LAYER_DRIVER;
    return XPUSHARE_LAYER_DRIVER;
  }

  if (!strcasecmp(e, "driver")) {
    g_enforcement_layer = (int)XPUSHARE_LAYER_DRIVER;
  } else if (!strcasecmp(e, "runtime")) {
    g_enforcement_layer = (int)XPUSHARE_LAYER_RUNTIME;
  } else if (!strcasecmp(e, "none") || !strcasecmp(e, "off") || !strcasecmp(e, "disable") ||
             !strcasecmp(e, "disabled")) {
    g_enforcement_layer = (int)XPUSHARE_LAYER_NONE;
  } else if (!strcasecmp(e, "profile") || !strcasecmp(e, "profiling")) {
    g_enforcement_layer = (int)XPUSHARE_LAYER_PROFILE;
  } else {
    g_enforcement_layer = (int)XPUSHARE_LAYER_DRIVER;
  }

  return (XpushareEnforcementLayer)g_enforcement_layer;
}
 
static bool driver_enforcement_enabled() {
  if (g_xpushare_in_runtime_shim) {
    return false;
  }
  return xpushare_enforcement_layer() == XPUSHARE_LAYER_DRIVER;
}
 
 static bool force_req_quota_enabled() {
   if (g_force_req_quota < 0) {
     const char *e = getenv("XHOOK_FORCE_REQ_QUOTA");
     g_force_req_quota = (e && e[0] == '1') ? 1 : 0;
   }
   return g_force_req_quota == 1;
 }
 
 static bool extra_debug_enabled() {
   if (g_extra_debug < 0) {
     const char *e = getenv("XHOOK_EXTRA_DEBUG");
     g_extra_debug = (e && e[0] == '1') ? 1 : 0;
   }
   return g_extra_debug == 1;
 }
 //
 
 // GPU memory allocation information
 pthread_mutex_t allocation_mutex = PTHREAD_MUTEX_INITIALIZER;
 std::map<CUdeviceptr, size_t> allocation_map;
 size_t gpu_mem_used = 0;  // local accounting only
 
 /**
  * get elapsed us since certain time
  * @param begin starting time
  * @return microseconds since starting time
  */
 long long us_since(struct timespec begin) {
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (now.tv_sec - begin.tv_sec) * 1000000LL + (now.tv_nsec - begin.tv_nsec) / 1000LL;
 }
 
 /**
  * get connection information from environment variables
  */
 // void save_port_number(){
 //   // get Pod manager port, default 50052
 //   char *port = getenv("POD_MANAGER_PORT");
 //   if (port != NULL) pod_manager_port = atoi(port);
 
 //   std::ofstream ofs(scheduler_port_file);
 //   if(!ofs.is_open()){
 //     hERROR(log_name, __FILE__, (long)__LINE__, "Failed to open the port file");
 //     exit(-1);
 //   }
 //   ofs << port;
 //   ofs.close();
 // }
 /**
  * get connection information from file
  */
 void configure_connection() {
   // get Pod manager IP, default 127.0.0.1
   /*char *ip = getenv("POD_MANAGER_IP");
   if (ip != NULL) strcpy(pod_manager_ip, ip);
   */
  
   std::ifstream ifs_ip(scheduler_ip_file, std::ios::in);
   if(!ifs_ip.is_open()){
     hERROR(log_name, __FILE__, (long)__LINE__, "Failed to open the ip file");
     exit(-1);
   }
   std::string line;
   getline(ifs_ip,line);
   if(line != "") strcpy(pod_manager_ip, line.c_str());
   ifs_ip.close();
   // get Pod manager port, default 50052
  /* std::ifstream ifs_port(scheduler_port_file, std::ios::in);
   if(!ifs_port.is_open()){
     hERROR(log_name, __FILE__, (long)__LINE__, "Failed to open the port file");
     exit(-1);
   }
   getline(ifs_port,line);
   if(line!="") pod_manager_port = stoi(line);
   ifs_port.close();*/
   char *port = getenv("POD_MANAGER_PORT");
   if (port != NULL) pod_manager_port = atoi(port);
 
   hDEBUG(log_name, __FILE__, (long)__LINE__, "Pod manager: %s:%u", pod_manager_ip, pod_manager_port);
   hINFO(log_name, __FILE__, (long)__LINE__, "Pod manager: %s:%u", pod_manager_ip, pod_manager_port);
 }
 
 int attempt_connection(int __fd, __CONST_SOCKADDR_ARG __addr, socklen_t __len) {
   configure_connection();
   return connect(__fd, __addr, __len);
 }
 /**
  * establish connection with scheduler.
  * @return connected socket file descriptor
  */
 int establish_connection() {
   configure_connection();
 
   int sockfd = socket(AF_INET, SOCK_STREAM, 0);
   if (sockfd == -1) {
     hERROR(log_name, __FILE__, (long)__LINE__, "Failed to create socket.");
     exit(-1);
   }
 
   struct sockaddr_in info;
   bzero(&info, sizeof(info));
   info.sin_family = PF_INET;
   info.sin_addr.s_addr = inet_addr(pod_manager_ip);
   info.sin_port = htons(pod_manager_port);
 
   // connect(sockfd, (struct sockaddr *)&info, sizeof(info));
   int rc = multiple_attempt(
       [&]() -> int { return attempt_connection(sockfd, (struct sockaddr *)&info, sizeof(info));},
       NET_OP_MAX_ATTEMPT, NET_OP_RETRY_INTV);
   if (rc != 0) {
     hERROR(log_name, __FILE__, (long)__LINE__, "Connection error: %s", strerror(rc));
     exit(rc);
   }
 
   return sockfd;
 }
 
 /**
  * Unified communication method with Pod manager/scheduler.
  * Send a request and receive a response.
  * Only one thread can communicate at the same time.
  * @param sbuf buffer with the data to send.
  * @param rbuf buffer which will be filled with received data.
  * @param socket_timeout socket timeout (second), 0 means never timeout
  * @return buffer with received data
  */
 int communicate(char *sbuf, char *rbuf, int socket_timeout) {
   static int sockfd = establish_connection();
   int rc;
   struct timeval tv;
 
   pthread_mutex_lock(&comm_mutex);
 
   // set socket timeout
   tv = {socket_timeout, 0};
   setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
 
  // perform communication
  rc = multiple_attempt(
      [&]() -> int {
        if (send(sockfd, sbuf, REQ_MSG_LEN, 0) == -1){
          hDEBUG(log_name, __FILE__, (long)__LINE__, "multiple_attempt send error %s", strerror(errno));
          return -1;
        }
        ssize_t rlen = recv(sockfd, rbuf, RSP_MSG_LEN, 0);
        if (rlen <= 0){
          hDEBUG(log_name, __FILE__, (long)__LINE__, "multiple_attempt recv error: %s (rlen=%zd)",
                 rlen == 0 ? "connection closed" : strerror(errno), rlen);
          return -1;
        }
        return 0;
      },
      NET_OP_MAX_ATTEMPT);
 
   pthread_mutex_unlock(&comm_mutex);
   return rc;
 }
 
 /**
  * Record a host-side synchronous call and update predictor statistics.
  * @param func_name name of synchronous call
  */
 void host_sync_call(const char *func_name) {
 #ifdef SYNCP_MESSAGE
   hDEBUG(log_name, __FILE__, (long)__LINE__, "SYNC (%s)", func_name);
 #endif
   burst_predictor.record_stop();
   window_predictor.record_start();
 }
 
 /**
  * get available GPU memory from Pod manager/scheduler
  * assume user memory limit won't exceed hardware limit
  * @return remaining memory, memory limit
  */
 std::pair<size_t, size_t> get_gpu_memory_info() {
   char sbuf[REQ_MSG_LEN], rbuf[RSP_MSG_LEN], *attached;
   size_t rpos = 0;
   int rc;
   size_t used, total;
 
   bzero(sbuf, REQ_MSG_LEN);
   prepare_request(sbuf, REQ_MEM_LIMIT);
 
   // get data from Pod manager
   rc = communicate(sbuf, rbuf, NET_OP_RETRY_INTV);
   if (rc != 0) {
     hERROR(log_name, __FILE__, (long)__LINE__, "failed to get GPU memory information: %s", strerror(rc));
     exit(rc);
   }
   attached = parse_response(rbuf, nullptr);
   used = get_msg_data<size_t>(attached, rpos);
   total = get_msg_data<size_t>(attached, rpos);
 
   return std::make_pair(total - used, total);
 }
 
 /**
  * send memory allocate/free information to Pod manager/scheduler
  * @param bytes memory size
  * @param is_allocate 1 for allocation, 0 for free
  * @return request succeed or not
  */
 int update_memory_usage(size_t bytes, int is_allocate) {
   char sbuf[REQ_MSG_LEN], rbuf[RSP_MSG_LEN], *attached;
   size_t rpos = 0;
   int rc;
   int verdict;
 
   bzero(sbuf, REQ_MSG_LEN);
   prepare_request(sbuf, REQ_MEM_UPDATE, bytes, is_allocate);
 
   // get verdict from Pod manager
   rc = communicate(sbuf, rbuf, 0);
   if (rc != 0) {
     hERROR(log_name, __FILE__, (long)__LINE__, "failed to update GPU memory usage: %s", strerror(rc));
     exit(rc);
   }
   attached = parse_response(rbuf, nullptr);
   verdict = get_msg_data<int>(attached, rpos);
 
   return verdict;
 }
 
 /**
  * Estimate the length of a complete burst — v2.
  *
  * The original blindly doubled the burst estimate when the window was
  * shorter than SCHD_OVERHEAD (2ms).  This caused massive over-estimation
  * for applications with short, frequent bursts (e.g., inference workloads),
  * because the window between bursts is naturally very short.
  *
  * New approach: use the predictor's EMA-based output directly.  The new
  * RecordKeeper already provides a smoothed prediction with moderate
  * headroom (EMA + 0.5 * headroom).  We only add a small safety margin
  * (10%) when the window is very short, instead of doubling.
  */
 double estimate_full_burst(double measured_burst, double measured_window) {
   double full_burst;

   if (measured_burst < 1e-9) {
     full_burst = 0.0;
   } else {
     full_burst = measured_burst;
     // Small safety margin when bursts are nearly continuous.
     // The old code used *2 here which was far too aggressive.
     if (measured_window < SCHD_OVERHEAD) {
       full_burst *= 1.1;
     }
   }

   hDEBUG(log_name, __FILE__, (long)__LINE__,
          "burst: %.3f ms, window: %.3f ms, estimated: %.3f ms",
          measured_burst, measured_window, full_burst);
   return full_burst;
 }
 
 /**
  * send token request to scheduling system
  * @param next_burst predicted kernel burst (milliseconds)
  * @return received time quota (milliseconds)
  */
  double get_token_from_scheduler(double next_burst) {
   char sbuf[REQ_MSG_LEN], rbuf[RSP_MSG_LEN], *attached;
   size_t rpos = 0;
   int rc;
   double new_quota;
 
   if (extra_debug_enabled()) {
     hINFO(log_name, __FILE__, (long)__LINE__,
           "[REQ_QUOTA] get_token_from_scheduler enter, overuse=%.3fms, next_burst=%.3fms",
           overuse, next_burst);
   }
 
   bzero(sbuf, REQ_MSG_LEN);
   bzero(rbuf, RSP_MSG_LEN);
   prepare_request(sbuf, REQ_QUOTA, overuse, next_burst);
 
   rc = communicate(sbuf, rbuf, 0);
   if (rc != 0) {
     hERROR(log_name, __FILE__, (long)__LINE__,
            "[REQ_QUOTA] failed to get token from scheduler: %s", strerror(rc));
     exit(rc);
   }
   attached = parse_response(rbuf, nullptr);
   new_quota = get_msg_data<double>(attached, rpos);
 
   hINFO(log_name, __FILE__, (long)__LINE__,
         "[REQ_QUOTA] token received, quota_time=%.3fms", new_quota);
   return new_quota;
 }
 
 /**
  * wait for all active kernels to complete, and update overuse statistics. note that cuda default
  * stream has an additional characteristic of implicit synchronization, which roughly means that a
  * CUDA operation issued into the default stream will not begin executing until all prior issued
  * CUDA activity to that device has completed. (from https://stackoverflow.com/a/49331700 by Robert
  * Crovella)
  * @param args not in use now
  */
 void *wait_cuda_kernels(void *args) {
   struct timespec ts;
   double nsec;
   while (true) {
     // wait for tracking request
     pthread_mutex_lock(&overuse_trk_mutex);
     pthread_cond_wait(&overuse_trk_strt_cond, &overuse_trk_mutex);
     pthread_mutex_unlock(&overuse_trk_mutex);
 
     // calculate token expiration time
     nsec = std::max(quota_time, (double)0.0) * 1e6;
     clock_gettime(CLOCK_MONOTONIC, &ts);
     ts.tv_sec += floor(nsec / 1e9);
     ts.tv_nsec += (nsec - floor(nsec / 1e9) * 1e9);
     ts.tv_sec += ts.tv_nsec / 1000000000;
     ts.tv_nsec %= 1000000000;
 
     // sleep until token expired or being notified
     pthread_mutex_lock(&overuse_trk_mutex);
     int rc = pthread_cond_timedwait(&overuse_trk_intr_cond, &overuse_trk_mutex, &ts);
     if (rc != ETIMEDOUT) {
       hDEBUG(log_name, __FILE__, (long)__LINE__, "overuse tracking thread interrupted");
     }
     pthread_mutex_unlock(&overuse_trk_mutex);
 
     // synchronize all running kernels
     cudaEvent_t event;
     cudaEventCreate(&event);
     cudaEventRecord(event);
     cudaEventSynchronize(event);
 
     // notify predictor we've done a synchronize
     host_sync_call("overuse measurement");
 
     float elapsed_ms;
     cudaEventElapsedTime(&elapsed_ms, cuevent_start, event);
     overuse = std::max((double)0.0, (double)elapsed_ms - quota_time);
 
     hDEBUG(log_name, __FILE__, (long)__LINE__, "overuse: %.3f ms", overuse);
     // notify tracking complete
     pthread_mutex_lock(&overuse_trk_mutex);
     overuse_trk_cmpl = true;
     pthread_cond_broadcast(&overuse_trk_cmpl_cond);
     pthread_mutex_unlock(&overuse_trk_mutex);
   }
   pthread_exit(NULL);
 }
 
 
 //11-27添加
 // 通用的“kernel 即将 launch”调度逻辑，供 Driver prehook 和 Runtime shim 共用
 static void on_kernel_launch_request() {
   double new_quota = 0.0, next_burst = 0.0;
 
   window_predictor.record_stop();
 
   if (extra_debug_enabled()) {
     hINFO(log_name, __FILE__, (long)__LINE__,
           "[SCHED] on_kernel_launch_request enter, quota_time=%.3f, ongoing_unmerged=%d",
           quota_time, (int)burst_predictor.ongoing_unmerged());
   }
 
   pthread_mutex_lock(&expiration_status_mutex);
 
   bool need_new_token = false;
   double host_elapsed_ms = us_since(request_start) / 1e3;
 
   if (force_req_quota_enabled()) {
     // 调试模式：每个 kernel 都强制申请一次配额
     need_new_token = true;
   } else if (!burst_predictor.ongoing_unmerged() &&
              host_elapsed_ms + burst_predictor.predict_unmerged() >= quota_time) {
     // 原始条件：burst 已结束且预测时间超过配额
     need_new_token = true;
   } else if (host_elapsed_ms >= quota_time) {
     // 兜底条件：即使 burst_predictor 认为 burst 仍在进行中（ongoing_unmerged==true），
     // 只要 host 时间已经超过 quota_time，也必须强制请求新 token。
     // 这解决了 CoreX 上 cudaEventSynchronize 阻塞导致 record_stop() 永远不被调用、
     // ongoing_unmerged() 永远为 true、客户端永远不再请求 token 的问题。
     if (extra_debug_enabled()) {
       hINFO(log_name, __FILE__, (long)__LINE__,
             "[SCHED] host-time fallback: host_elapsed=%.3fms >= quota_time=%.3fms, "
             "ongoing_unmerged=%d, forcing new token request",
             host_elapsed_ms, quota_time, (int)burst_predictor.ongoing_unmerged());
     }
     // 强制结束当前 burst，使 predictor 状态恢复正常
     burst_predictor.record_stop();
     need_new_token = true;
   }
 
   if (need_new_token) {
     next_burst =
         estimate_full_burst(burst_predictor.predict_merged(), window_predictor.predict_merged());
 
     if (extra_debug_enabled()) {
       hINFO(log_name, __FILE__, (long)__LINE__,
             "[SCHED] need new token, predicted next_burst=%.3fms", next_burst);
     }
 
     // wait for all kernels finish (with timeout to handle CoreX cudaEventSynchronize hang)
     pthread_mutex_lock(&overuse_trk_mutex);
     if (!overuse_trk_cmpl) {
       pthread_cond_signal(&overuse_trk_intr_cond);
       // 使用带超时的等待，防止 overuse tracking 线程卡在 cudaEventSynchronize 上
       // 导致主线程永远阻塞。超时时间 = max(quota_time, 500ms)。
       struct timespec cmpl_ts;
       double wait_ms = ((double)quota_time > 500.0) ? (double)quota_time : 500.0;
       clock_gettime(CLOCK_REALTIME, &cmpl_ts);
       cmpl_ts.tv_sec  += (long)(wait_ms / 1e3);
       cmpl_ts.tv_nsec += (long)(fmod(wait_ms, 1e3) * 1e6);
       cmpl_ts.tv_sec  += cmpl_ts.tv_nsec / 1000000000;
       cmpl_ts.tv_nsec %= 1000000000;
       int wait_rc = pthread_cond_timedwait(&overuse_trk_cmpl_cond, &overuse_trk_mutex, &cmpl_ts);
       if (wait_rc == ETIMEDOUT) {
         // overuse tracking 线程可能卡在 cudaEventSynchronize，用 host 时间估算 overuse
         double est = host_elapsed_ms - (double)quota_time;
         overuse = (est > 0.0) ? est : 0.0;
         overuse_trk_cmpl = true;
         hINFO(log_name, __FILE__, (long)__LINE__,
               "[SCHED] overuse tracking timed out (%.0fms), estimated overuse=%.3fms",
               wait_ms, overuse);
       }
     }
     pthread_mutex_unlock(&overuse_trk_mutex);
 
     window_predictor.interrupt();
 
     new_quota = get_token_from_scheduler(next_burst);
 
     burst_predictor.set_upperbound(new_quota - 1.0);
 
     cudaEventRecord(cuevent_start, 0);
     clock_gettime(CLOCK_MONOTONIC, &request_start);
 
     quota_time = new_quota;
 
     pthread_mutex_lock(&overuse_trk_mutex);
     overuse_trk_cmpl = false;
     pthread_cond_signal(&overuse_trk_strt_cond);
     pthread_mutex_unlock(&overuse_trk_mutex);
   } else {
     if (extra_debug_enabled()) {
       hINFO(log_name, __FILE__, (long)__LINE__,
             "[SCHED] no new token needed for this kernel, quota_time=%.3f", quota_time);
     }
   }
 
   burst_predictor.record_start();
   pthread_mutex_unlock(&expiration_status_mutex);
 }
 
 
 /**
  * pre-hooks and post-hooks
  */
  CUresult cuLaunchKernel_prehook(CUfunction f, unsigned int gridDimX, unsigned int gridDimY,
   unsigned int gridDimZ, unsigned int blockDimX,
   unsigned int blockDimY, unsigned int blockDimZ,
   unsigned int sharedMemBytes, CUstream hStream, void **kernelParams,
   void **extra) {
 // 这些参数目前在调度逻辑里没有被使用，避免编译器告警
   (void)f;
   (void)gridDimX; (void)gridDimY; (void)gridDimZ;
   (void)blockDimX; (void)blockDimY; (void)blockDimZ;
   (void)sharedMemBytes; (void)hStream;
   (void)kernelParams; (void)extra;
 
   if (driver_enforcement_enabled()) {
     on_kernel_launch_request();
   }
   return CUDA_SUCCESS;
 }
 
 /*CUresult cuLaunchKernel_prehook(CUfunction f, unsigned int gridDimX, unsigned int gridDimY,
                                 unsigned int gridDimZ, unsigned int blockDimX,
                                 unsigned int blockDimY, unsigned int blockDimZ,
                                 unsigned int sharedMemBytes, CUstream hStream, void **kernelParams,
                                 void **extra) {
             
   double new_quota, next_burst;
 
   window_predictor.record_stop();
 
   pthread_mutex_lock(&expiration_status_mutex);
   // allow the kernel to launch if kernel burst already begins;
   // otherwise, obtain a new token if this kernel burst may cause overuse
   if (!burst_predictor.ongoing_unmerged() &&
       us_since(request_start) / 1e3 + burst_predictor.predict_unmerged() >= quota_time) {
     // estimate the duration of next kernel burst (merged)
     next_burst =
         estimate_full_burst(burst_predictor.predict_merged(), window_predictor.predict_merged());
 
     // wait for all kernels finish
     pthread_mutex_lock(&overuse_trk_mutex);
     if (!overuse_trk_cmpl) {
       // notify overuse tracking thread to perform sync eariler
       pthread_cond_signal(&overuse_trk_intr_cond);
       pthread_cond_wait(&overuse_trk_cmpl_cond, &overuse_trk_mutex);
     }
     pthread_mutex_unlock(&overuse_trk_mutex);
 
     // interrupt the window which is started when overuse tracking completes
     window_predictor.interrupt();
 
     new_quota = get_token_from_scheduler(next_burst);
 
     // ensure predicted kernel burst is always less than quota
     burst_predictor.set_upperbound(new_quota - 1.0);
 
     cudaEventRecord(cuevent_start, 0);
     clock_gettime(CLOCK_MONOTONIC, &request_start);  // time
 
     quota_time = new_quota;
 
     // wake overuse tracking thread up
     pthread_mutex_lock(&overuse_trk_mutex);
     overuse_trk_cmpl = false;
     pthread_cond_signal(&overuse_trk_strt_cond);
     pthread_mutex_unlock(&overuse_trk_mutex);
   }
   burst_predictor.record_start();
   pthread_mutex_unlock(&expiration_status_mutex);
 
   return CUDA_SUCCESS;
 }*/
 
 
 /*
 CUresult cuLaunchCooperativeKernel_prehook(CUfunction f, unsigned int gridDimX,
                                            unsigned int gridDimY, unsigned int gridDimZ,
                                            unsigned int blockDimX, unsigned int blockDimY,
                                            unsigned int blockDimZ, unsigned int sharedMemBytes,
                                            CUstream hStream, void **kernelParams) {
   return cuLaunchKernel_prehook(f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ,
                                 sharedMemBytes, hStream, kernelParams, NULL);
 }*/
 
 // update memory usage
 CUresult cuMemFree_prehook(CUdeviceptr ptr) {
   if (!driver_enforcement_enabled()) {
     return CUDA_SUCCESS;
   }
   pthread_mutex_lock(&allocation_mutex);
   if (allocation_map.find(ptr) == allocation_map.end()) {
     hDEBUG(log_name, __FILE__, (long)__LINE__, "Freeing unknown memory! %zx", ptr);
   } else {
     gpu_mem_used -= allocation_map[ptr];
     update_memory_usage(allocation_map[ptr], 0);
     allocation_map.erase(ptr);
   }
   pthread_mutex_unlock(&allocation_mutex);
   return CUDA_SUCCESS;
 }
 
 CUresult cuArrayDestroy_prehook(CUarray hArray) { return cuMemFree_prehook((CUdeviceptr)hArray); }
 
 CUresult cuMipmappedArrayDestroy_prehook(CUmipmappedArray hMipmappedArray) {
   return cuMemFree_prehook((CUdeviceptr)hMipmappedArray);
 }
 
 // ask backend whether there's enough memory or not
 CUresult cuMemAlloc_prehook(CUdeviceptr *dptr, size_t bytesize) {
   if (!driver_enforcement_enabled()) {
     return CUDA_SUCCESS;
   }
   size_t remain, limit;
   std::tie(remain, limit) = get_gpu_memory_info();
 
   // block allocation request before over-allocate
   if (bytesize > remain) {
     hERROR(log_name, __FILE__, (long)__LINE__, "Allocate too much memory! (request: %lu B, remain: %lu B)", bytesize, remain);
     return CUDA_ERROR_OUT_OF_MEMORY;
   }
 
   return CUDA_SUCCESS;
 }
 
 // push memory allocation information to backend
 CUresult cuMemAlloc_posthook(CUdeviceptr *dptr, size_t bytesize) {
   if (!driver_enforcement_enabled()) {
     return CUDA_SUCCESS;
   }
   // send memory usage update to backend
   if (!update_memory_usage(bytesize, 1)) {
     hERROR(log_name, __FILE__, (long)__LINE__, "Allocate too much memory!");
     return CUDA_ERROR_OUT_OF_MEMORY;
   }
 
   pthread_mutex_lock(&allocation_mutex);
   allocation_map[*dptr] = bytesize;
   gpu_mem_used += bytesize;
   pthread_mutex_unlock(&allocation_mutex);
 
   return CUDA_SUCCESS;
 }
 
 CUresult cuMemAllocManaged_prehook(CUdeviceptr *dptr, size_t bytesize, unsigned int flags) {
   // TODO: This function access the unified memory. Behavior needs clarification.
   return CUDA_SUCCESS;
 }
 
 CUresult cuMemAllocManaged_posthook(CUdeviceptr *dptr, size_t bytesize, unsigned int flags) {
   // TODO: This function access the unified memory. Behavior needs clarification.
   return CUDA_SUCCESS;
 }
 
 CUresult cuMemAllocPitch_prehook(CUdeviceptr *dptr, size_t *pPitch, size_t WidthInBytes,
                                  size_t Height, unsigned int ElementSizeBytes) {
   return cuMemAlloc_prehook(dptr, (*pPitch) * Height);
 }
 CUresult cuMemAllocPitch_posthook(CUdeviceptr *dptr, size_t *pPitch, size_t WidthInBytes,
                                   size_t Height, unsigned int ElementSizeBytes) {
   return cuMemAlloc_posthook(dptr, (*pPitch) * Height);
 }
 
 inline size_t CUarray_format_to_size_t(CUarray_format Format) {
   switch (Format) {
     case CU_AD_FORMAT_UNSIGNED_INT8:
     case CU_AD_FORMAT_SIGNED_INT8:
       return 1;
     case CU_AD_FORMAT_UNSIGNED_INT16:
     case CU_AD_FORMAT_SIGNED_INT16:
     case CU_AD_FORMAT_HALF:
       return 2;
     case CU_AD_FORMAT_UNSIGNED_INT32:
     case CU_AD_FORMAT_SIGNED_INT32:
     case CU_AD_FORMAT_FLOAT:
       return 4;
   }
 }
 
 CUresult cuArrayCreate_prehook(CUarray *pHandle, const CUDA_ARRAY_DESCRIPTOR *pAllocateArray) {
   size_t totalMemoryNumber =
       pAllocateArray->Width * pAllocateArray->Height * pAllocateArray->NumChannels;
   size_t formatSize = CUarray_format_to_size_t(pAllocateArray->Format);
   return cuMemAlloc_prehook((CUdeviceptr *)pHandle, totalMemoryNumber * formatSize);
 }
 
 CUresult cuArrayCreate_posthook(CUarray *pHandle, const CUDA_ARRAY_DESCRIPTOR *pAllocateArray) {
   size_t totalMemoryNumber =
       pAllocateArray->Width * pAllocateArray->Height * pAllocateArray->NumChannels;
   size_t formatSize = CUarray_format_to_size_t(pAllocateArray->Format);
   return cuMemAlloc_posthook((CUdeviceptr *)pHandle, totalMemoryNumber * formatSize);
 }
 /*
 CUresult cuArray3DCreate_prehook(CUarray *pHandle, const CUDA_ARRAY3D_DESCRIPTOR *pAllocateArray) {
   size_t totalMemoryNumber = pAllocateArray->Width * pAllocateArray->Height *
                              pAllocateArray->Depth * pAllocateArray->NumChannels;
   size_t formatSize = CUarray_format_to_size_t(pAllocateArray->Format);
   return cuMemAlloc_prehook((CUdeviceptr *)pHandle, totalMemoryNumber * formatSize);
 }*/
 
 /*CUresult cuArray3DCreate_posthook(CUarray *pHandle, const CUDA_ARRAY3D_DESCRIPTOR *pAllocateArray) {
   size_t totalMemoryNumber = pAllocateArray->Width * pAllocateArray->Height *
                              pAllocateArray->Depth * pAllocateArray->NumChannels;
   size_t formatSize = CUarray_format_to_size_t(pAllocateArray->Format);
   return cuMemAlloc_posthook((CUdeviceptr *)pHandle, totalMemoryNumber * formatSize);
 }*/
 
 CUresult cuMipmappedArrayCreate_prehook(CUmipmappedArray *pHandle,
                                         const CUDA_ARRAY3D_DESCRIPTOR *pMipmappedArrayDesc,
                                         unsigned int numMipmapLevels) {
   // TODO: check mipmap array size
   return CUDA_SUCCESS;
 }
 
 CUresult cuMipmappedArrayCreate_posthook(CUmipmappedArray *pHandle,
                                          const CUDA_ARRAY3D_DESCRIPTOR *pMipmappedArrayDesc,
                                          unsigned int numMipmapLevels) {
   // TODO: check mipmap array size
   return CUDA_SUCCESS;
 }
 
 CUresult cuCtxSynchronize_posthook(void) {
   host_sync_call("cuCtxSynchronize");
   return CUDA_SUCCESS;
 }
 
 CUresult cuMemcpyAtoH_posthook(void *dstHost, CUarray srcArray, size_t srcOffset,
                                size_t ByteCount) {
   host_sync_call("cuMemcpyAtoH");
   return CUDA_SUCCESS;
 }
 
 CUresult cuMemcpyDtoH_posthook(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount) {
   host_sync_call("cuMemcpyDtoH");
   return CUDA_SUCCESS;
 }
 
 CUresult cuMemcpyHtoA_posthook(CUarray dstArray, size_t dstOffset, const void *srcHost,
                                size_t ByteCount) {
   host_sync_call("cuMemcpyHtoA");
   return CUDA_SUCCESS;
 }
 
 CUresult cuMemcpyHtoD_posthook(CUarray dstArray, size_t dstOffset, const void *srcHost,
                                size_t ByteCount, CUstream hStream) {
   host_sync_call("cuMemcpyHtoD");
   return CUDA_SUCCESS;
 }
 
 void initialize() {
 
   if (hook_inf.debug_mode) {
     printf("--- initialize() was called (PID: %d) ---\n", getpid());
   }
   fflush(stdout);
   // place post-hooks
   hook_inf.postHooks[CU_HOOK_MEMCPY_ATOH] = (void *)cuMemcpyAtoH_posthook;
   hook_inf.postHooks[CU_HOOK_MEMCPY_DTOH] = (void *)cuMemcpyDtoH_posthook;
   hook_inf.postHooks[CU_HOOK_MEMCPY_HTOA] = (void *)cuMemcpyHtoA_posthook;
   hook_inf.postHooks[CU_HOOK_MEMCPY_HTOD] = (void *)cuMemcpyHtoD_posthook;
   hook_inf.postHooks[CU_HOOK_CTX_SYNC] = (void *)cuCtxSynchronize_posthook;
 
   hook_inf.postHooks[CU_HOOK_MEM_ALLOC] = (void *)cuMemAlloc_posthook;
   hook_inf.postHooks[CU_HOOK_MEM_ALLOC_MANAGED] = (void *)cuMemAllocManaged_posthook;
   hook_inf.postHooks[CU_HOOK_MEM_ALLOC_PITCH] = (void *)cuMemAllocPitch_posthook;
   hook_inf.postHooks[CU_HOOK_ARRAY_CREATE] = (void *)cuArrayCreate_posthook;
   //hook_inf.postHooks[CU_HOOK_ARRAY3D_CREATE] = (void *)cuArray3DCreate_posthook;
   hook_inf.postHooks[CU_HOOK_MIPMAPPED_ARRAY_CREATE] = (void *)cuMipmappedArrayCreate_posthook;
   // place pre-hooks
   hook_inf.preHooks[CU_HOOK_MEM_FREE] = (void *)cuMemFree_prehook;
   hook_inf.preHooks[CU_HOOK_ARRAY_DESTROY] = (void *)cuArrayDestroy_prehook;
   hook_inf.preHooks[CU_HOOK_MIPMAPPED_ARRAY_DESTROY] = (void *)cuMipmappedArrayDestroy_prehook;
   hook_inf.preHooks[CU_HOOK_LAUNCH_KERNEL] = (void *)cuLaunchKernel_prehook;
   //hook_inf.preHooks[CU_HOOK_LAUNCH_COOPERATIVE_KERNEL] = (void *)cuLaunchCooperativeKernel_prehook;
 
   hook_inf.preHooks[CU_HOOK_MEM_ALLOC] = (void *)cuMemAlloc_prehook;
   hook_inf.preHooks[CU_HOOK_MEM_ALLOC_MANAGED] = (void *)cuMemAllocManaged_prehook;
   hook_inf.preHooks[CU_HOOK_MEM_ALLOC_PITCH] = (void *)cuMemAllocPitch_prehook;
   hook_inf.preHooks[CU_HOOK_ARRAY_CREATE] = (void *)cuArrayCreate_prehook;
   //hook_inf.preHooks[CU_HOOK_ARRAY3D_CREATE] = (void *)cuArray3DCreate_prehook;
   hook_inf.preHooks[CU_HOOK_MIPMAPPED_ARRAY_CREATE] = (void *)cuMipmappedArrayCreate_prehook;
   //save_port_number();
   configure_connection();
   pthread_mutex_lock(&request_time_mutex);
   cudaEventCreate(&cuevent_start);
 
   // initialize overuse_trk_intr_cond with CLOCK_MONOTONIC
   pthread_condattr_t attr_monotonic_clock;
   pthread_condattr_init(&attr_monotonic_clock);
   pthread_condattr_setclock(&attr_monotonic_clock, CLOCK_MONOTONIC);
   pthread_cond_init(&overuse_trk_intr_cond, &attr_monotonic_clock);
 
   // a thread running overuse tracking
   pthread_t overuse_trk_tid;
   pthread_create(&overuse_trk_tid, NULL, wait_cuda_kernels, NULL);
 
   // first token request
   overuse_trk_cmpl = true;  // bypass first overuse tracking to prevent deadlock
   get_token_from_scheduler(0.0);
 
   pthread_mutex_unlock(&request_time_mutex);
 }
 
 CUstream hStream;  // redundent variable used for macro expansion
 //generate hook for cuda < 11.3
 #define CU_HOOK_GENERATE_INTERCEPT(hook_name, hooksymbol, funcname, params, ...)                     \
   CUresult CUDAAPI hook_name params {                                                      \
     if (hook_inf.debug_mode) hDEBUG(log_name, __FILE__, (long)__LINE__, "hooked function: " CUDA_SYMBOL_STRING(hooksymbol));   \
     pthread_once(&init_done, initialize);                                                 \
                                                                                           \
     static void *real_func = NULL;                                                 \
     char v2_name[256];                                                             \
     snprintf(v2_name, 256, "%s_v2", CUDA_SYMBOL_STRING(funcname));                  \
     real_func = (void *)dlsym(RTLD_NEXT, v2_name);                                 \
     if (real_func == NULL) {                                                       \
         real_func = (void *)dlsym(RTLD_NEXT, CUDA_SYMBOL_STRING(funcname));         \
     }                                                                              \
     if (real_func == NULL && hook_inf.debug_mode) {                                \
         hDEBUG(log_name, __FILE__, (long)__LINE__, "dlsym failed to find symbol: %s or %s", CUDA_SYMBOL_STRING(funcname), v2_name); \
     }                                                                              \
     CUresult result = CUDA_SUCCESS;                                                     \
     if (hook_inf.coverage_mode) hook_inf.call_count[hooksymbol]++;                        \
                                                                                           \
     if (hook_inf.preHooks[hooksymbol])                                                    \
       result = ((CUresult CUDAAPI(*) params)hook_inf.preHooks[hooksymbol])(__VA_ARGS__);  \
     if (result != CUDA_SUCCESS) return (result);                                          \
                                                                                           \
     result = ((CUresult CUDAAPI(*) params)real_func)(__VA_ARGS__);                        \
                                                                                           \
     if (hook_inf.postHooks[hooksymbol] && result == CUDA_SUCCESS)                         \
       result = ((CUresult CUDAAPI(*) params)hook_inf.postHooks[hooksymbol])(__VA_ARGS__); \
                                                                                           \
     return (result);                                                                      \
   }                                                                                       
 
 CU_HOOK_GENERATE_INTERCEPT(hook_cuMemcpyAtoH, CU_HOOK_MEMCPY_ATOH, cuMemcpyAtoH,
                            (void *dstHost, CUarray srcArray, size_t srcOffset, size_t ByteCount),
                            dstHost, srcArray, srcOffset, ByteCount)
 CU_HOOK_GENERATE_INTERCEPT(hook_cuMemcpyDtoH, CU_HOOK_MEMCPY_DTOH, cuMemcpyDtoH,
                            (void *dstHost, CUdeviceptr srcDevice, size_t ByteCount), dstHost,
                            srcDevice, ByteCount)
 CU_HOOK_GENERATE_INTERCEPT(hook_cuMemcpyHtoA, CU_HOOK_MEMCPY_HTOA, cuMemcpyHtoA,
                            (CUarray dstArray, size_t dstOffset, const void *srcHost,
                             size_t ByteCount),
                            dstArray, dstOffset, srcHost, ByteCount)
 CU_HOOK_GENERATE_INTERCEPT(hook_cuMemcpyHtoD, CU_HOOK_MEMCPY_HTOD, cuMemcpyHtoD,
                            (CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount),
                            dstDevice, srcHost, ByteCount)
 CU_HOOK_GENERATE_INTERCEPT(hook_cuCtxSynchronize, CU_HOOK_CTX_SYNC, cuCtxSynchronize, (void))
 
 // cuda driver alloc/free APIs
 CU_HOOK_GENERATE_INTERCEPT(hook_cuMemAlloc, CU_HOOK_MEM_ALLOC, cuMemAlloc, (CUdeviceptr * dptr, size_t bytesize),
                            dptr, bytesize)
 CU_HOOK_GENERATE_INTERCEPT(hook_cuMemAllocManaged, CU_HOOK_MEM_ALLOC_MANAGED, cuMemAllocManaged,
                            (CUdeviceptr * dptr, size_t bytesize, unsigned int flags), dptr,
                            bytesize, flags)
 CU_HOOK_GENERATE_INTERCEPT(hook_cuMemAllocPitch, CU_HOOK_MEM_ALLOC_PITCH, cuMemAllocPitch,
                            (CUdeviceptr * dptr, size_t *pPitch, size_t WidthInBytes, size_t Height,
                             unsigned int ElementSizeBytes),
                            dptr, pPitch, WidthInBytes, Height, ElementSizeBytes)
 CU_HOOK_GENERATE_INTERCEPT(hook_cuMemFree, CU_HOOK_MEM_FREE, cuMemFree, (CUdeviceptr dptr), dptr)
 
 // cuda driver array/array_destroy APIs
 CU_HOOK_GENERATE_INTERCEPT(hook_cuArrayCreate, CU_HOOK_ARRAY_CREATE, cuArrayCreate,
                            (CUarray * pHandle, const CUDA_ARRAY_DESCRIPTOR *pAllocateArray),
                            pHandle, pAllocateArray)
 /*
 CU_HOOK_GENERATE_INTERCEPT(hook_cuArray3DCreate, CU_HOOK_ARRAY3D_CREATE, cuArray3DCreate,
                            (CUarray * pHandle, const CUDA_ARRAY3D_DESCRIPTOR *pAllocateArray),
                            pHandle, pAllocateArray)*/
 
 CU_HOOK_GENERATE_INTERCEPT(hook_cuMipmappedArrayCreate, CU_HOOK_MIPMAPPED_ARRAY_CREATE, cuMipmappedArrayCreate,
                            (CUmipmappedArray * pHandle,
                             const CUDA_ARRAY3D_DESCRIPTOR *pMipmappedArrayDesc,
                             unsigned int numMipmapLevels),
                            pHandle, pMipmappedArrayDesc, numMipmapLevels)
 CU_HOOK_GENERATE_INTERCEPT(hook_cuArrayDestroy, CU_HOOK_ARRAY_DESTROY, cuArrayDestroy, (CUarray hArray), hArray)
 CU_HOOK_GENERATE_INTERCEPT(hook_cuMipmappedArrayDestroy, CU_HOOK_MIPMAPPED_ARRAY_DESTROY, cuMipmappedArrayDestroy,
                            (CUmipmappedArray hMipmappedArray), hMipmappedArray)
 
 // cuda driver kernel launch APIs
 CU_HOOK_GENERATE_INTERCEPT(hook_cuLaunchKernel, CU_HOOK_LAUNCH_KERNEL, cuLaunchKernel,
                            (CUfunction f, unsigned int gridDimX, unsigned int gridDimY,
                             unsigned int gridDimZ, unsigned int blockDimX, unsigned int blockDimY,
                             unsigned int blockDimZ, unsigned int sharedMemBytes, CUstream hStream,
                             void **kernelParams, void **extra),
                            f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ,
                            sharedMemBytes, hStream, kernelParams, extra)
 /*CU_HOOK_GENERATE_INTERCEPT(hook_cuLaunchCooperativeKernel, CU_HOOK_LAUNCH_COOPERATIVE_KERNEL, cuLaunchCooperativeKernel,
                            (CUfunction f, unsigned int gridDimX, unsigned int gridDimY,
                             unsigned int gridDimZ, unsigned int blockDimX, unsigned int blockDimY,
                             unsigned int blockDimZ, unsigned int sharedMemBytes, CUstream hStream,
                             void **kernelParams),
                            f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ,
                            sharedMemBytes, hStream, kernelParams)*/
 
 // cuda driver mem info APIs
 extern "C" {
 static const char *kHookSymbolNames[NUM_HOOK_SYMBOLS] = {
     "cuGetProcAddress",
     "cuMemAlloc",
     "cuMemAllocManaged",
     "cuMemAllocPitch",
     "cuMemFree",
     "cuArrayCreate",
     "cuArray3DCreate",
     "cuMipmappedArrayCreate",
     "cuArrayDestroy",
     "cuMipmappedArrayDestroy",
     "cuCtxGetCurrent",
     "cuCtxSetCurrent",
     "cuCtxDestroy",
     "cuLaunchKernel",
     "cuLaunchCooperativeKernel",
     "cuDeviceTotalMem",
     "cuMemGetInfo",
     "cuCtxSynchronize",
     "cuMemcpyAtoH",
     "cuMemcpyDtoH",
     "cuMemcpyHtoA",
     "cuMemcpyHtoD",
 };
 
 static int g_coverage_dumped = 0;
 static void dump_hook_coverage_once() {
   if (!hook_inf.coverage_mode) {
     return;
   }
   if (__sync_lock_test_and_set(&g_coverage_dumped, 1)) {
     return;
   }
 
   hINFO(log_name, __FILE__, (long)__LINE__, "[COVERAGE][driver] begin pid=%d", getpid());
   for (int i = 0; i < (int)NUM_HOOK_SYMBOLS; i++) {
     const char *name = (i >= 0 && i < (int)NUM_HOOK_SYMBOLS) ? kHookSymbolNames[i] : "unknown";
     if (!name) {
       name = "unknown";
     }
     hINFO(log_name, __FILE__, (long)__LINE__, "[COVERAGE][driver] %s=%d", name, hook_inf.call_count[i]);
   }
   hINFO(log_name, __FILE__, (long)__LINE__, "[COVERAGE][driver] end pid=%d", getpid());
 }
 
 __attribute__((destructor)) static void dump_hook_coverage_destructor() {
   dump_hook_coverage_once();
 }
 
 CUresult CUDAAPI cuDeviceTotalMem(size_t *bytes, CUdevice dev) {
   pthread_once(&init_done, initialize);
   if (hook_inf.coverage_mode) hook_inf.call_count[CU_HOOK_DEVICE_TOTOAL_MEM]++;
 
  XpushareEnforcementLayer layer = xpushare_enforcement_layer();
  if (layer == XPUSHARE_LAYER_PROFILE || layer == XPUSHARE_LAYER_NONE) {
     static real_fn_t real_fn = NULL;
     if (!real_fn) {
       real_fn = (real_fn_t)dlsym(RTLD_NEXT, "cuDeviceTotalMem");
     }
     if (real_fn) {
       return real_fn(bytes, dev);
     }
     return CUDA_ERROR_NOT_INITIALIZED;
   }
 
   auto mem_info = get_gpu_memory_info();
   *bytes = mem_info.second;
   return CUDA_SUCCESS;
 }
 
 CUresult CUDAAPI cuMemGetInfo(size_t *gpu_mem_free, size_t *gpu_mem_total) {
   if (hook_inf.debug_mode) {
     printf("--- libxhook.so.1 loaded (PID: %d) ---\n", getpid());
     fflush(stdout);
   }
 
   pthread_once(&init_done, initialize);
   if (hook_inf.coverage_mode) hook_inf.call_count[CU_HOOK_MEM_INFO]++;
 
  XpushareEnforcementLayer layer = xpushare_enforcement_layer();
  if (layer == XPUSHARE_LAYER_PROFILE || layer == XPUSHARE_LAYER_NONE) {
    typedef CUresult(CUDAAPI *real_fn_t)(size_t *, size_t *);
     static real_fn_t real_fn = NULL;
     if (!real_fn) {
       real_fn = (real_fn_t)dlsym(RTLD_NEXT, "cuMemGetInfo");
     }
     if (real_fn) {
       return real_fn(gpu_mem_free, gpu_mem_total);
     }
     return CUDA_ERROR_NOT_INITIALIZED;
   }
 
   auto mem_info = get_gpu_memory_info();
   *gpu_mem_free = mem_info.first;
   *gpu_mem_total = mem_info.second;
   return CUDA_SUCCESS;
 }
 
 
 CUresult CUDAAPI cuGetProcAddress(const char *symbol, void **pfn, int cudaVersion, cuuint64_t flags) {
   if (libcudaHandle == NULL) {
       libcudaHandle = dlopen("libcuda.so", RTLD_LAZY);
       if (libcudaHandle == NULL) {
           // 你可以在此添加错误日志，例如使用 hERROR
           // hERROR(log_name, __FILE__, (long)__LINE__, "FATAL: Failed to dlopen libcuda.so");
           // 此时无法继续，可以返回一个错误
           return CUDA_ERROR_NOT_INITIALIZED;
       }
   }
 
   typedef void *(*fnDlsym)(void *, const char *);
   fnDlsym internal_dlsym = (fnDlsym)dlsym(RTLD_NEXT, "dlsym");
   if (internal_dlsym == NULL) {
       // 无法获取 dlsym，返回错误
       return CUDA_ERROR_NOT_INITIALIZED;
   }
 
   typedef decltype(&cuGetProcAddress) funcType;
   funcType actualFunc = nullptr;
   if (!hook_inf.func_actual[CU_HOOK_GET_PROC_ADDRESS]) {
       actualFunc = (funcType)internal_dlsym(libcudaHandle, CUDA_SYMBOL_STRING(cuGetProcAddress));
   } else {
       actualFunc = (funcType)hook_inf.func_actual[CU_HOOK_GET_PROC_ADDRESS];
   }
 
   CUresult result = CUDA_ERROR_UNKNOWN;
   if (actualFunc) {
       result = actualFunc(symbol, pfn, cudaVersion, flags);
   } else {
       return CUDA_ERROR_NOT_INITIALIZED;
   }
 
   if (strcmp(symbol, CUDA_SYMBOL_STRING(cuGetProcAddress)) == 0) {
       hook_inf.func_actual[CU_HOOK_GET_PROC_ADDRESS] = *pfn;
       *pfn = (void*)(&cuGetProcAddress);
 
 #pragma push_macro("cuMemAlloc")
 #undef cuMemAlloc
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMemAlloc)) == 0 || strcmp(symbol, "cuMemAlloc_v2") == 0) {
 #pragma pop_macro("cuMemAlloc")
       hook_inf.func_actual[CU_HOOK_MEM_ALLOC] = *pfn;
       *pfn = (void *)(&hook_cuMemAlloc);
 
 #pragma push_macro("cuMemAllocManaged")
 #undef cuMemAllocManaged
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMemAllocManaged)) == 0 || strcmp(symbol, "cuMemAllocManaged_v2") == 0) {
 #pragma pop_macro("cuMemAllocManaged")
       hook_inf.func_actual[CU_HOOK_MEM_ALLOC_MANAGED] = *pfn;
       *pfn = (void *)(&hook_cuMemAllocManaged);
 
 #pragma push_macro("cuMemAllocPitch")
 #undef cuMemAllocPitch
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMemAllocPitch)) == 0 || strcmp(symbol, "cuMemAllocPitch_v2") == 0) {
 #pragma pop_macro("cuMemAllocPitch")
       hook_inf.func_actual[CU_HOOK_MEM_ALLOC_PITCH] = *pfn;
       *pfn = (void *)(&hook_cuMemAllocPitch);
 
 #pragma push_macro("cuMemFree")
 #undef cuMemFree
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMemFree)) == 0 || strcmp(symbol, "cuMemFree_v2") == 0) {
 #pragma pop_macro("cuMemFree")
       hook_inf.func_actual[CU_HOOK_MEM_FREE] = *pfn;
       *pfn = (void *)(&hook_cuMemFree);
 
 #pragma push_macro("cuCtxSynchronize")
 #undef cuCtxSynchronize
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuCtxSynchronize)) == 0) {
 #pragma pop_macro("cuCtxSynchronize")
       hook_inf.func_actual[CU_HOOK_CTX_SYNC] = *pfn;
       *pfn = (void *)(&hook_cuCtxSynchronize);
 
 #pragma push_macro("cuMemcpyHtoD")
 #undef cuMemcpyHtoD
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMemcpyHtoD)) == 0 || strcmp(symbol, "cuMemcpyHtoD_v2") == 0) {
 #pragma pop_macro("cuMemcpyHtoD")
       hook_inf.func_actual[CU_HOOK_MEMCPY_HTOD] = *pfn;
       *pfn = (void *)(&hook_cuMemcpyHtoD);
 
 #pragma push_macro("cuMemcpyDtoH")
 #undef cuMemcpyDtoH
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMemcpyDtoH)) == 0 || strcmp(symbol, "cuMemcpyDtoH_v2") == 0) {
 #pragma pop_macro("cuMemcpyDtoH")
       hook_inf.func_actual[CU_HOOK_MEMCPY_DTOH] = *pfn;
       *pfn = (void *)(&hook_cuMemcpyDtoH);
 
 #pragma push_macro("cuMemcpyAtoH")
 #undef cuMemcpyAtoH
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMemcpyAtoH)) == 0 || strcmp(symbol, "cuMemcpyAtoH_v2") == 0) {
 #pragma pop_macro("cuMemcpyAtoH")
       hook_inf.func_actual[CU_HOOK_MEMCPY_ATOH] = *pfn;
       *pfn = (void *)(&cuMemcpyAtoH);
 
 #pragma push_macro("cuMemcpyHtoA")
 #undef cuMemcpyHtoA
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMemcpyHtoA)) == 0 || strcmp(symbol, "cuMemcpyHtoA_v2") == 0) {
 #pragma pop_macro("cuMemcpyHtoA")
       hook_inf.func_actual[CU_HOOK_MEMCPY_HTOA] = *pfn;
       *pfn = (void *)(&hook_cuMemcpyHtoA);
 
 #pragma push_macro("cuLaunchKernel")
 #undef cuLaunchKernel
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuLaunchKernel)) == 0) {
 #pragma pop_macro("cuLaunchKernel")
       hook_inf.func_actual[CU_HOOK_LAUNCH_KERNEL] = *pfn;
       *pfn = (void *)(&hook_cuLaunchKernel);
 
 #pragma push_macro("cuLaunchCooperativeKernel")
 #undef cuLaunchCooperativeKernel
   /*
    * 下面是被你注释掉的 cuLaunchCooperativeKernel 替换块（成对注释，语法安全）。
    * 如需启用，请取消此注释并确保 hook_cuLaunchCooperativeKernel 存在且正确。
    *
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuLaunchCooperativeKernel)) == 0) {
 #pragma pop_macro("cuLaunchCooperativeKernel")
       hook_inf.func_actual[CU_HOOK_LAUNCH_COOPERATIVE_KERNEL] = *pfn;
       *pfn = (void *)(&hook_cuLaunchCooperativeKernel);
   */
 #pragma push_macro("cuArrayCreate")
 #undef cuArrayCreate
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuArrayCreate)) == 0 || strcmp(symbol, "cuArrayCreate_v2") == 0) {
 #pragma pop_macro("cuArrayCreate")
       hook_inf.func_actual[CU_HOOK_ARRAY_CREATE] = *pfn;
       *pfn = (void *)(&hook_cuArrayCreate);
 
 #pragma push_macro("cuArray3DCreate")
 #undef cuArray3DCreate
   /*
    * 原代码中你把 cuArray3DCreate 的分支部分注释掉。我把整个分支用成对注释包住，保持语法正确。
    *
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuArray3DCreate)) == 0) {
 #pragma pop_macro("cuArray3DCreate")
       hook_inf.func_actual[CU_HOOK_ARRAY3D_CREATE] = *pfn;
       *pfn = (void *)(&hook_cuArray3DCreate);
   */
 #pragma push_macro("cuMipmappedArrayCreate")
 #undef cuMipmappedArrayCreate
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMipmappedArrayCreate)) == 0) {
 #pragma pop_macro("cuMipmappedArrayCreate")
       hook_inf.func_actual[CU_HOOK_MIPMAPPED_ARRAY_CREATE] = *pfn;
       *pfn = (void *)(&hook_cuMipmappedArrayCreate);
 
 #pragma push_macro("cuArrayDestroy")
 #undef cuArrayDestroy
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuArrayDestroy)) == 0) {
 #pragma pop_macro("cuArrayDestroy")
       hook_inf.func_actual[CU_HOOK_ARRAY_DESTROY] = *pfn;
       *pfn = (void *)(&hook_cuArrayDestroy);
 
 #pragma push_macro("cuMipmappedArrayDestroy")
 #undef cuMipmappedArrayDestroy
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMipmappedArrayDestroy)) == 0) {
 #pragma pop_macro("cuMipmappedArrayDestroy")
       hook_inf.func_actual[CU_HOOK_MIPMAPPED_ARRAY_DESTROY] = *pfn;
       *pfn = (void *)(&cuMipmappedArrayDestroy);
 
 #pragma push_macro("cuDeviceTotalMem")
 #undef cuDeviceTotalMem
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuDeviceTotalMem)) == 0 || strcmp(symbol, "cuDeviceTotalMem_v2") == 0) {
 #pragma pop_macro("cuDeviceTotalMem")
       hook_inf.func_actual[CU_HOOK_DEVICE_TOTOAL_MEM] = *pfn;
       *pfn = (void *)(&cuDeviceTotalMem);
 
 #pragma push_macro("cuMemGetInfo")
 #undef cuMemGetInfo
   } else if (strcmp(symbol, CUDA_SYMBOL_STRING(cuMemGetInfo)) == 0 || strcmp(symbol, "cuMemGetInfo_v2") == 0) {
 #pragma pop_macro("cuMemGetInfo")
       hook_inf.func_actual[CU_HOOK_MEM_INFO] = *pfn;
       *pfn = (void *)(&cuMemGetInfo);
   }
 
 #ifdef _DEBUG
   //printf("Leave %s\n", SYMBOL_STRING(cuGetProcAddress));
 #endif
 
   return (result);
 }
 
 
 
 //generate hook for cuda >= 11.3
 #define CU_HOOK_GENERATE_INTERCEPT_v1(hooksymbol, funcname, params, ...)                     \
   CUresult CUDAAPI funcname params {                                                      \
     if (hook_inf.debug_mode) hDEBUG(log_name, __FILE__, (long)__LINE__, "hooked function: " CUDA_SYMBOL_STRING(hooksymbol));   \
     pthread_once(&init_done, initialize);                                                 \
                                                                                           \
     static void *real_func = (void *)dlsym(RTLD_NEXT, CUDA_SYMBOL_STRING(funcname)); \
     CUresult result = CUDA_SUCCESS;                                                       \
                                                                                           \
     if (hook_inf.coverage_mode) hook_inf.call_count[hooksymbol]++;                        \
                                                                                           \
     if (hook_inf.preHooks[hooksymbol])                                                    \
       result = ((CUresult CUDAAPI(*) params)hook_inf.preHooks[hooksymbol])(__VA_ARGS__);  \
     if (result != CUDA_SUCCESS) return (result);                                          \
                                                                                           \
     result = ((CUresult CUDAAPI(*) params)real_func)(__VA_ARGS__);                        \
                                                                                           \
     if (hook_inf.postHooks[hooksymbol] && result == CUDA_SUCCESS)                         \
       result = ((CUresult CUDAAPI(*) params)hook_inf.postHooks[hooksymbol])(__VA_ARGS__); \
                                                                                           \
     return (result);                                                                      \
   }
 
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_MEMCPY_ATOH, cuMemcpyAtoH,
                            (void *dstHost, CUarray srcArray, size_t srcOffset, size_t ByteCount),
                            dstHost, srcArray, srcOffset, ByteCount)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_MEMCPY_DTOH, cuMemcpyDtoH,
                            (void *dstHost, CUdeviceptr srcDevice, size_t ByteCount), dstHost,
                            srcDevice, ByteCount)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_MEMCPY_HTOA, cuMemcpyHtoA,
                            (CUarray dstArray, size_t dstOffset, const void *srcHost,
                             size_t ByteCount),
                            dstArray, dstOffset, srcHost, ByteCount)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_MEMCPY_HTOD, cuMemcpyHtoD,
                            (CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount),
                            dstDevice, srcHost, ByteCount)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_CTX_SYNC, cuCtxSynchronize, (void))
 
 // cuda driver alloc/free APIs
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_MEM_ALLOC, cuMemAlloc, (CUdeviceptr * dptr, size_t bytesize),
                            dptr, bytesize)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_MEM_ALLOC_MANAGED, cuMemAllocManaged,
                            (CUdeviceptr * dptr, size_t bytesize, unsigned int flags), dptr,
                            bytesize, flags)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_MEM_ALLOC_PITCH, cuMemAllocPitch,
                            (CUdeviceptr * dptr, size_t *pPitch, size_t WidthInBytes, size_t Height,
                             unsigned int ElementSizeBytes),
                            dptr, pPitch, WidthInBytes, Height, ElementSizeBytes)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_MEM_FREE, cuMemFree, (CUdeviceptr dptr), dptr)
 
 // cuda driver array/array_destroy APIs
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_ARRAY_CREATE, cuArrayCreate,
                            (CUarray * pHandle, const CUDA_ARRAY_DESCRIPTOR *pAllocateArray),
                            pHandle, pAllocateArray)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_ARRAY3D_CREATE, cuArray3DCreate,
                            (CUarray * pHandle, const CUDA_ARRAY3D_DESCRIPTOR *pAllocateArray),
                            pHandle, pAllocateArray)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_MIPMAPPED_ARRAY_CREATE, cuMipmappedArrayCreate,
                            (CUmipmappedArray * pHandle,
                             const CUDA_ARRAY3D_DESCRIPTOR *pMipmappedArrayDesc,
                             unsigned int numMipmapLevels),
                            pHandle, pMipmappedArrayDesc, numMipmapLevels)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_ARRAY_DESTROY, cuArrayDestroy, (CUarray hArray), hArray)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_MIPMAPPED_ARRAY_DESTROY, cuMipmappedArrayDestroy,
                            (CUmipmappedArray hMipmappedArray), hMipmappedArray)
 
 // cuda driver kernel launch APIs
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_LAUNCH_KERNEL, cuLaunchKernel,
                            (CUfunction f, unsigned int gridDimX, unsigned int gridDimY,
                             unsigned int gridDimZ, unsigned int blockDimX, unsigned int blockDimY,
                             unsigned int blockDimZ, unsigned int sharedMemBytes, CUstream hStream,
                             void **kernelParams, void **extra),
                            f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ,
                            sharedMemBytes, hStream, kernelParams, extra)
 CU_HOOK_GENERATE_INTERCEPT_v1(CU_HOOK_LAUNCH_COOPERATIVE_KERNEL, cuLaunchCooperativeKernel,
                            (CUfunction f, unsigned int gridDimX, unsigned int gridDimY,
                             unsigned int gridDimZ, unsigned int blockDimX, unsigned int blockDimY,
                             unsigned int blockDimZ, unsigned int sharedMemBytes, CUstream hStream,
                             void **kernelParams),
                            f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ,
                            sharedMemBytes, hStream, kernelParams)
 
 
 // ===================================================
 // 在 hook.cpp 暴露给 shim.c 的 C 接口
 // ===================================================
 // Shim 在调用 cudaMalloc 前，通过此函数获取剩余显存
 size_t get_cuda_free_mem_for_shim(void) {
   size_t remain, limit;
   std::tie(remain, limit) = get_gpu_memory_info();
   (void)limit;
   return remain;
 }
 
 int update_mem_usage_for_shim(size_t bytes, int is_allocate) {
   return update_memory_usage(bytes, is_allocate);
 }
 
 // 关键：供 shim 在发射 kernel 前调用，触发 REQ_QUOTA
 int request_launch_token_for_shim(void) {
   // 先确保 initialize() 已执行：连接 pod-manager、起 tracking 线程、拿第一个 token 等
   pthread_once(&init_done, initialize);
 
   if (extra_debug_enabled()) {
     hINFO(log_name, __FILE__, (long)__LINE__, "[SCHED] request_launch_token_for_shim called");
   }
 
   on_kernel_launch_request();
 
   return 1; // 目前总是允许执行
 }
 
 // Shim 用于 cudaGetDeviceProperties
 size_t get_total_memory_limit_for_shim(void) {
   auto info = get_gpu_memory_info();
   return info.second;
 }
 
 // Shim 用于 cudaMemGetInfo
 void get_mem_info_for_shim(size_t *free_bytes, size_t *total_bytes) {
   auto info = get_gpu_memory_info();
   if (free_bytes)  *free_bytes  = info.first;
   if (total_bytes) *total_bytes = info.second;
 }
 
 // Shim 用于 cudaDeviceSynchronize / cudaStreamSynchronize
 // 通知 burst predictor 发生了一次 host-GPU 同步（burst 结束）
 void host_sync_call_for_shim(const char *func_name) {
   host_sync_call(func_name);
 }
 }
 
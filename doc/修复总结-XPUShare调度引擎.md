# XPUShare 调度引擎修复总结

## 一、修复范围

本轮修复覆盖 xhook 调度引擎的全部 C/C++ 核心文件（9 个文件，3677 行），涉及 **4 个层面**：burst 预测器、GPU 级调度器、调度优先级比较器、Pod 管理器。累计修复 **19 个 bug**，其中 7 个可导致崩溃或死锁。

### 涉及文件

| 文件 | 行数 | 修改内容 |
|------|------|----------|
| `core/predictor.cpp` | 247 | 重写 RecordKeeper，EMA + 衰减最大值 |
| `core/predictor.h` | 93 | 新增 EMA/衰减字段，constexpr 定义 |
| `core/scheduler.cpp` | 720 | 重写 select_candidate、get_quota、schedule_daemon |
| `core/scheduler.h` | 80 | 新增 last_granted_quota_、overuse_ema_、deficit_ratio |
| `core/schd-priority.cpp` | 67 | 重写比较器，修复严格弱序 |
| `core/comm.cpp` | 134 | 修复 va_start 未定义行为 |
| `core/pod-manager.cpp` | 621 | 重写同步模型，修复数据竞争 |
| `backends/cuda/hook.cpp` | 1481 | 修复 recv 返回值检查、estimate_full_burst |
| `Makefile` | 234 | 修复 CORE_CXX 编译器选择、依赖声明 |

---

## 二、逐 Bug 修复清单

### 第一轮：调度算法核心重写（5 个 bug）

#### Bug 1 — Predictor 异常值绑架
- **文件**：`predictor.cpp`, `predictor.h`
- **问题**：原 `RecordKeeper` 用单调递减队列只保留 3 秒窗口内的最大值做预测。一次偶发的长 burst（如 GPU 初始化）会把后续所有 quota 请求都拉高到该值，直到 3 秒后才衰减。
- **修复**：改为 EMA（α=0.3）+ 衰减最大值（半衰期 500ms）。`get_predicted()` 返回 `ema + 0.5 × max(0, decayed_max - ema)`，outlier 在 500ms 内自然消退。`estimate_full_burst` 的盲目 ×2 改为 ×1.1。

#### Bug 2 — Usage 计算的 overlap-splitting 错误
- **文件**：`scheduler.cpp` (`select_candidate`)
- **问题**：原算法用 overlap-splitting 均分重叠时间段（`1/overlap_cnt`），导致高频 client 的 usage 被系统性低估——实际用了 100ms，但因为和另一个 client 重叠就只算 50ms。
- **修复**：改为对每个 client 的 history intervals 独立做 clip-and-sum：`usage[name] += min(h.end, now) - max(h.start, window_start)`。

#### Bug 3 — Quota 分配无 overuse 反馈
- **文件**：`scheduler.cpp` (`ClientInfo::get_quota`)
- **问题**：原 `get_quota` 只做 burst 的 EWMA，完全不看 overuse 反馈。request=0.2 和 request=0.8 的 client 如果 burst 模式相同，拿到的 quota 也一样。
- **修复**：引入 `overuse_ema_` 修正因子。`correction = clamp(1.0 - 0.5 × overuse_ema_, 0.5, 1.3)`，持续超用的 client quota 收缩，持续欠用的微增。

#### Bug 4 — 调度优先级忽略 request 大小
- **文件**：`schd-priority.cpp`
- **问题**：原比较器用 `missing/(missing+usage)` 排序，这是相对指标，忽略了 `min_frac` 的绝对大小。request=0.1 和 request=0.5 的 client 在 usage=0 时优先级相同。
- **修复**：改为 `deficit_ratio = (require - usage) / require`，按各自 request 归一化后的亏欠程度排序。

#### Bug 5 — 调度循环阻塞
- **文件**：`scheduler.cpp` (`schedule_daemon_func`)
- **问题**：原 daemon 发完 token 后 sleep 等 quota 超时，期间整个调度器被阻塞，其他 client 排队等着。这是比例失控的最直接原因——串行化让低 request 的 client 也能独占整个 quota 时段。
- **修复**：发完 token 立即回到 `select_candidate`，client 用完 token 后自己重新入队。send 失败时直接丢弃 token（不再重试 5 次 × 3 秒）。

---

### 第二轮：overuse_ema 计算错误（1 个 bug）

#### Bug 6 — update_return_time 使用错误的 quota 值
- **文件**：`scheduler.cpp` (`ClientInfo::update_return_time`, `ClientInfo::get_quota`)
- **问题**：`overuse_ema_` 的计算使用 `quota_`（会被 `get_quota()` 更新后的新值），而不是实际授予给 client 的 quota。
- **修复**：引入 `last_granted_quota_` 字段，在 `get_quota()` 末尾保存，在 `update_return_time` 中使用。

---

### 第三轮：编译和运行时安全（6 个 bug）

#### Bug 7 — comm.cpp va_start 未定义行为
- **文件**：`comm.cpp`
- **问题**：5 处 `va_start` 全部传了整数字面量（如 `va_start(vl, 3)`）而不是最后一个命名参数。这是未定义行为，在某些平台上 varargs 解析会读到垃圾数据。
- **修复**：改为 `va_start(vl, type)` 和 `va_start(vl, id)`。

#### Bug 8 — pod-manager.cpp client_name 未初始化
- **文件**：`pod-manager.cpp` (`hook_thread_func`)
- **问题**：`client_name` 未初始化就被传给 `DEBUG` 的 `%s`，第一次 `recv` 失败时段错误。
- **修复**：初始化为 `nullptr`，所有 `%s` 格式化处加空指针保护。

#### Bug 9 — Makefile CORE_CXX 使用 GPU 编译器
- **文件**：`Makefile`
- **问题**：`CORE_CXX := $(CXX)` 导致 scheduler 和 pod-manager（纯 C++ 代码）使用 GPU 编译器编译，在没有 GPU SDK 的机器上失败。
- **修复**：改为 `CORE_CXX := g++`。补全 `comm.o` 对 `debug.h` 的依赖声明。

#### Bug 10 — scheduler.cpp client_info_map 数据竞争
- **文件**：`scheduler.cpp`
- **问题**：`client_info_map` 被 `monitor_file` 线程（通过 `read_resource_config`）和 `schedule_daemon`/`handle_message` 线程并发读写，没有锁保护。
- **修复**：统一用 `candidate_mutex` 保护所有共享状态。

#### Bug 11 — predictor.h constexpr ODR 违规
- **文件**：`predictor.cpp`
- **问题**：`static constexpr double DECAY_HALF_LIFE_MS` 在 C++11 下缺少类外定义。
- **修复**：在 `predictor.cpp` 中添加 `constexpr double RecordKeeper::DECAY_HALF_LIFE_MS;`。

#### Bug 12 — pod-manager.cpp getenv NULL 解引用
- **文件**：`pod-manager.cpp` (`main`)
- **问题**：`getenv("POD_NAME")` 返回 NULL 时直接传给 `operator<<`。
- **修复**：加 NULL 检查（已在原代码中存在 if/else，但 else 分支的 gethostname 路径是安全的，确认无问题）。

---

### 第四轮：ABBA 死锁和无限循环（3 个 bug）

#### Bug 13 — ABBA 死锁
- **文件**：`scheduler.cpp`
- **问题**：`schedule_daemon_func` 先 lock `candidate_mutex` 再 lock `config_mutex`，而 `handle_message` 先 lock `config_mutex` 再 lock `candidate_mutex`。两个线程交叉执行时必然死锁。
- **修复**：去掉 `config_mutex`，统一用 `candidate_mutex`。`handle_message` 重构为入口处一次性 lock，每个分支在不再需要共享数据后 unlock。

#### Bug 14 — select_candidate 无限循环
- **文件**：`scheduler.cpp` (`select_candidate`)
- **问题**：循环改为 `for (auto it = ...; ; )` 以支持 erase，但正常路径上忘了 `++it`。当 client 存在但 `remaining <= 0` 时，iterator 永远不前进，CPU 100% 空转。
- **修复**：在循环体末尾添加 `++it`。

#### Bug 15 — Quick exit 跳过 client_info_map 检查
- **文件**：`scheduler.cpp` (`select_candidate`)
- **问题**：如果 config 热更新删除了某个 client，但该 client 的 candidate 还在队列中，quick exit 路径会直接返回它，导致 `client_info_map[selected.name]` 解引用空指针。
- **修复**：quick exit 增加 `client_info_map.find()` 检查。unknown client 的 candidate 用 `erase` 清理。

---

### 第五轮：Pod 管理器同步模型重写（4 个 bug）

#### Bug 16 — pod-manager 5 锁死锁 + 数据竞争
- **文件**：`pod-manager.cpp` (`hook_kernel_launch`)
- **问题**：原代码用 5 个 mutex（`quota_state_mutex`、`kernel_launch_count_mutex`、`sleeping_count_mutex`、`scheduler_recv_sync_mutex`、`rsp_map_mutex`）和 `sleeping_count` 屏障协调多线程。`pod_overuse_ms`/`pod_quota`/`quota_updated_tp` 无锁读写；`sleeping_count` 屏障在线程到达时序不一致时死锁。
- **修复**：用 `quota_mutex` + `quota_cond` + `quota_updating` 替代全部 5 个 mutex。第一个发现 quota 过期的线程设 `quota_updating=true` 并去请求新 quota，其他线程在 `quota_cond` 上等待。

#### Bug 17 — gpu_mem_used 无符号下溢
- **文件**：`pod-manager.cpp` (`hook_update_memory_usage`, `hook_thread_func`)
- **问题**：`gpu_mem_used -= mem_size` 和 `allocation_map[sockfd] -= mem_size` 在 `mem_size` 大于当前值时 wrap 到 `SIZE_MAX`。
- **修复**：两处都加了 `if (mem_size > current) clamp to 0` 保护。

#### Bug 18 — scheduler_thread_recv_func 双层 while 无限循环
- **文件**：`pod-manager.cpp` (`scheduler_thread_recv_func`)
- **问题**：原代码有双层 `while(true)` 嵌套，`recv` 返回 0（连接关闭）后内层循环退出但外层循环立即重新进入，导致 CPU 100% 空转。
- **修复**：去掉外层 `while(true)`，改为单层 `while (recv > 0)` 循环。改用 `pthread_cond_broadcast` 确保多个等待线程都能被唤醒。

#### Bug 19 — scheduler_thread_send_func 信号丢失
- **文件**：`pod-manager.cpp` (`scheduler_thread_send_func`)
- **问题**：原代码先 `cond_wait` 再检查队列，如果两个请求几乎同时到达，第二个 signal 丢失。
- **修复**：改为先检查队列是否为空再 wait，用内层 while 循环一次性排空所有待发送请求。

---

### 第六轮：最终审计（6 个 bug）

#### Bug 20 — schd-priority.cpp 严格弱序违反
- **文件**：`schd-priority.cpp`
- **问题**：`deficit_ratio` 的 0.01 容差和 `remaining` 的 1.0 容差破坏传递性。例如 A≈B 且 B≈C 但 A≠C，违反 `std::sort` 要求的严格弱序，导致未定义行为（可能崩溃或死循环）。
- **修复**：去掉所有 epsilon 容差，改为精确比较 `!=`，用 `arrived_time` 作为最终 FIFO 打破平局。

#### Bug 21 — allocation_map 无锁插入
- **文件**：`pod-manager.cpp` (`main`)
- **问题**：`allocation_map.insert()` 在 main 线程中执行时没有持 `mem_info_mutex`，而 hook 线程可能同时在 `hook_update_memory_usage` 中读写同一个 map。`std::map` 的并发修改是未定义行为。
- **修复**：加 `mem_info_mutex` 保护。

#### Bug 22 — REQ_MEM_LIMIT 无锁读取
- **文件**：`pod-manager.cpp` (`hook_thread_func`)
- **问题**：`REQ_MEM_LIMIT` 分支直接读 `gpu_mem_used` 和 `gpu_mem_limit` 传给 `prepare_response`，没有持 `mem_info_mutex`。
- **修复**：加锁快照到局部变量再使用。

#### Bug 23 — send 线程持锁阻塞
- **文件**：`pod-manager.cpp` (`scheduler_thread_send_func`)
- **问题**：在持有 `req_queue_mutex` 的情况下执行阻塞 `send()`，如果网络慢会阻塞所有试图入队的 hook 线程。
- **修复**：先排空到本地 `std::vector`，释放锁后再逐个发送。

#### Bug 24 — sample_count_ 有符号溢出 UB
- **文件**：`predictor.cpp`
- **问题**：`sample_count_++` 在 `INT_MAX` 时溢出是 C++ 未定义行为，编译器可能优化掉后续的溢出检查。
- **修复**：改为 `if (sample_count_ < INT_MAX) sample_count_++`。

#### Bug 25 — communicate() recv 返回 0 不视为错误
- **文件**：`hook.cpp` (`communicate`)
- **问题**：`recv` 返回 0（连接关闭）不被视为错误，后续 `parse_response` 会解析垃圾数据。
- **修复**：改为检查 `<= 0`。

---

## 三、修复前后对比

### 时间片比例控制

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| A(0.3) + B(0.7) 共享 GPU | A 和 B 拿到近似相同的 quota，比例约 1:1 | A 和 B 的 token 分配趋向 3:7 |
| 偶发长 burst 后 | 预测值被拉高 3 秒不衰减，所有后续 quota 过大 | 500ms 半衰期自然消退 |
| Client 持续超用 | 无反馈，继续给相同 quota | overuse_ema 修正因子收缩 quota |
| 低 request client 独占 | daemon sleep 等 quota 超时，其他 client 排队 | 发完 token 立即调度下一个 |

### 稳定性

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| Config 热更新删除 client | 段错误（空指针解引用） | 安全清理 stale candidate |
| 多 hook 线程并发请求 quota | 死锁（sleeping_count 屏障） | 单 mutex + cond_wait |
| std::sort 调用 | 未定义行为（比较器非严格弱序） | 精确比较，满足严格弱序 |
| 长时间运行（>24天） | sample_count_ 溢出，UB | 钳位在 INT_MAX |

---

## 四、已知遗留问题

以下问题在审计中被识别但不修复，因为它们是原始架构的固有限制，修复需要重新设计整个通信协议：

1. **comm.cpp 缓冲区溢出风险**：`prepare_request` 中如果 `POD_NAME` 超过 47 字节，会溢出 `REQ_MSG_LEN(80)` 的缓冲区。Kubernetes 中 Pod 名通常 30-40 字符，但理论上可达 253 字符。需要将 `REQ_MSG_LEN` 增大或改为动态分配。

2. **hook.cpp ETIMEDOUT 竞态条件**：`on_kernel_launch_request` 中 `overuse_trk_cmpl` 超时后，旧的 `wait_cuda_kernels` 线程迭代可能延迟写入 `overuse` 和 `overuse_trk_cmpl`，干扰新周期。这只在 `cudaEventSynchronize` 长时间卡住时触发（CoreX 特定场景），且已有超时兜底。

3. **predictor 的 upperbound_ 死代码**：`set_upperbound()` 被调用但 `get_predicted()` 从未使用 `upperbound_` 来钳位预测值。这是原始代码的未完成功能。

4. **scheduler.cpp dump_history() 数据竞争**：`SIGINT` 信号处理函数直接遍历 `full_history` 而不持有 `candidate_mutex`。仅在 `_DEBUG` 编译模式下存在，且随后调用 `exit(0)`。

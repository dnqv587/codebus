# 非 HTTP 债务轮（基础设施 Q 批 + fd 资源层专项）

Status: resolved

## Problem Statement

known-issues 登记簿中非 HTTP 的基础设施疑点（Q5–Q13、Q29、Q31、Q32）与 D8 长期挂账：
负 epoch 日界错位、数组表解析未实现、异步日志 shadow mutex 竞态、Socket 构造不校验/
析构 assert/close 非幂等、epoll 构造失败静默继续、API 拼写与比较符缺口、语义未文档化。

## Solution

经 grilling 逐项裁决（2026-11-24/25）后按批 A→B→C 实施，TDD 红→绿，seam 为各既有
单元测试目标（TimestampTest/ConfigTest/LogAppenderTest/CountDownLatchTest/AddressTest/
BlockingQueueTest/ThreadTest/ThreadPoolTest/SocketTest/EventDispatcherTest）。

## Implementation Decisions（裁决记录）

- **批 A 低风险修复**：Q11 toDay/nextDay floor 除法（dayIndex）；D8 asArrayTable 实现
  （严格语义：混入非 table 整体 nullopt）+启用 ArrayTable；Q31 移除 AsynAppendFile
  shadow mutex、锁域统一基类（消费者取出+落盘同临界区，D32 锁外落盘形态废止）；
  Q29 关闭（shared_ptr<Handle> 已保证生命周期，实测缓解）+Node 构造改 POD 按值+
  误导性 move 改拷贝+ValueOutlivesConfig 扩展复合类型；Q5 过时关闭（文件已删）。
- **批 B API/语义**：Q8 coutDown→countDown（9 处全内部）；Q10 Timestamp 补 ==/!=
  （static_assert 翻转为存在性锁定）；Q6 over/put 契约文档化（行为不变，终结信号
  不拒绝 put）；Q7 Address 默认=无效哨兵文档化+isValid()；Q9 线程回调异常=进程
  terminate 契约文档化+死亡测试（ADR-0002，不 catch 不隔离）。
- **批 C fd 专项（ADR-0001）**：Q32 Socket(Family,Type) 构造失败抛 SocketError；
  Q12 析构容忍 INVALID、close 幂等返 true（两死亡测试翻转正例）；**fd 0 边缘整体
  维持现状**（CreateSocket/isAvailable/操作 assert 均 >0，用户裁决，Q12 部分闭环）；
  Q13 epoll 构造失败 LOG_FATAL+SystemUtil::Abort（兑现 EventLoopThread 既有声明，
  非抛异常）；UdpServer/UdpClient M9 兜底移除；TcpClient.h 析构契约注释同步。
- **不做**：D36 维持登记不修裁决；Observer.hpp 暂不删（用户裁决）；UDP W3 后置
  独立轮；操作内 assert 不改。
- **登记**：GLOSSARY.md 创建（哨兵地址/Over/排空）；ADR-0001/0002；known-issues
  逐项闭环（Q12 部分）。

## Testing Decisions

- 每项红→绿（Q31 红面在 TSAN 矩阵：unique_ptr reset 竞态 3/3；Q9/Q13 为契约/死亡
  测试锁定；Q29 为回归锁定）。
- rlimit 软限 0 逼 EMFILE（FdCount()-1 因 fd 表空洞不可靠，已修正）。
- 终验：常规/TSAN/ASAN 全量各 62/62；变体切换强制重链；Socket 地基影响面
  （TCP/UDP/EventLoop 系）单独回归。

## Out of Scope

- H5 治理轮全部功能；UDP W3 用例矩阵；死代码删除；fd 0 边缘修复；D36。

## Comments

- 2026-11-25 实现完成：三矩阵 62/62（常规 75.28s / TSAN 82.19s / ASAN 67.41s），
  常规变体已恢复。待 code-review 与按模块提交。
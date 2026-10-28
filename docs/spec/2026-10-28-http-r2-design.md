# HTTP 模块二期（H2）设计与计划（七单元 · 交互式）

> 状态：已批准（2026-10-28），实施中
> 关联：[一期设计](2026-10-27-http-design.md)（V0–V6+审查修复轮已闭环，提交 66e0402..c851921）

## 1. 需求背景

HTTP/1.1 Server 一期闭环。H2 目标：**先广度后深度**——协议补全（Client）、REST 路由（路径参数）、
服务标配（Session/静态文件/AccessLog）、防护加固与观察项清偿（Q33/Q35/Q36）；响应写路径的
通用流式/异步（架构分水岭）留 H3。

**架构审视结论**：一期骨架健康（分层单向/C API 隔离/生命周期纪律/三矩阵）；张力集中在响应写
路径（resp 栈对象 × Dump 定长 CL）——H2 以最小形态绕开（setBodyFile 场景 CL 已知），不做通用
占位接口。

## 2. 已裁决设计决策（用户确认 2026-10-28）

| # | 决策点 | 裁决 |
|---|---|---|
| 1 | H2 范围 | **7 单元一轮**（U7 弹性位，余量不足顺延不阻塞） |
| 2 | U5 流式响应 | **纳入最小形态**（setBodyFile+DumpHead；通用流式 H3） |
| 3 | HTTP Client | **异步复用 TcpClient**（Connector 建连+EventLoop 驱动+回调式响应） |
| 4 | 三项形态修正 | **全部采纳**：AccessLog 内建收尾点（Filter 信息论不可行——签名内拿不到实发字节/远端地址）/删 `*.js` 前缀通配（保留尾段 `*`）/gzip 移出 H3（zlib 依赖） |

我职责内定（方案审阅已呈）：U2 模式段匹配表（非段树，pattern<100）+`getPathParam` 独立接口
（mutable 装配，Servlet 签名零改动）；U5 DumpHead 拆分字节序全等；U7 leftover 精确透出
（修复一期丢弃边界后字节缺陷）。

## 3. 复用的既有资产与纪律

- 一期全栈（协议对象/parser 封装模式/容器/传输）；TcpClient+Connector（U3 基座）；
  UdpClient 的装配器公共约定先例；log 模块 appender 体系（U6 `http.access` logger 零新基建）
- D39/D40/C1 生命周期纪律；单飞+回灌；三 sanitizer 矩阵；变体切换强制重链
- TcpServer `handleNewConnection` 在 base loop 单线程持 m_conn_map——U1 连接上限天然无锁插入点

## 4. 处理单元设计（U1–U7）

（内容与已批准实施计划一致：U1 防护加固包 / U2 路由模式升级 / U3 异步 HTTP Client+
HttpResponseParser / U4 Session / U5 流式最小形态+StaticFileServlet / U6 AccessLog 内建收尾点 /
U7 WS 接管钩子；接口签名级定义见实施计划，此处不赘）

每单元：问题卡/方案确认 → 代码确认 before/after → TDD 红 → 实现 → 三矩阵验证 → 登记。

## 5. 交互式处理协议

同一期：每单元一轮六步；U5（最高风险）第 1 步须呈全量 diff；U1 涉及 TCP 底座改动，
TCP 12 目标先行回归。

## 6. 验证策略

| 时机 | 动作 |
|---|---|
| 每单元 | `ctest -R Http` + 全量回归 |
| U1 后 | TCP 12 目标先行 + 三矩阵 |
| U3/U5 后 | TSAN/ASAN 全矩阵 |
| 终验 | N×{常规,TSAN,ASAN} + repeat（新目标 HttpClient/Session/StaticFile） |
| 新目标 | 49 → 预计 52 |

## 7. 风险与对策

R1 U3 异步状态机（单在途 CAS+超时即断连宁丢不错位+断连恢复用例）；R2 U5 流式×单飞
（语义边界文档化+截断即关+定向用例）；R3 U1 TcpServer 签名变更（默认参零破坏+TCP 先行回归）；
R4 U2 mutable 装配（装配区 Doxygen+TSAN）；R5 总量超节奏（U7 可弃）。

## 8. 进度记录

| 单元 | 状态 | 备注 |
|---|---|---|
| U1 防护加固包 | **完成**（2026-10-28） | 常规 49/49 + ASAN/TSAN 7 目标绿（零泄漏零警告）；实施修正：测试 CL 笔误（user=tom+1 为 10 字节）经探针定位；DateLocale 断言反向自纠；TcpServerTest 用例补 Stop 契约 |
| U2 路由模式升级 | **完成**（2026-10-28） | 常规 49/49 一次全绿（13 新用例）；实现内合理化：registerMethod 自动识别 pattern 语法（Get("/items/{id}") 直接可用，MethodRouter 与模式组合 405 照常） |
| U3 异步 Client | **完成**（2026-10-28） | 常规 50/50（新目标 HttpClientTest 12 用例）；实施修正：①~HttpClient 跨线程析构 TimerManager（TimerManager loop 纪律）——Stop 清理改 RunInLoop+持 m_loop②HEAD 响应须 on_headers_complete 返回 1（C 库响应侧语义）③断连用例揭示 Stop 排队语义（单 loop 下 Stop 等在途 handler）——改 Reset 半关语义用例，硬断 nullopt 由 ResponseTimeout 覆盖④TSAN 4 处竞争修复：CallbackLatch 改 shared_ptr（闭包保活）+TearDown RunInLoopSync 析构屏障 |
| U4 Session | **完成**（2026-10-28） | 常规 51/51（SessionTest 10 用例）；实施修正：①Session 构造改 public（make_shared 与友元不兼容，Doxygen 标注）②Cookie 断言走 Dump 序列化产物（addCookie 独立容器，getHeader 不可见——语义澄清）③Filter attach 用 manager.config()（配置单一来源） |
| U5 流式+静态文件 | **完成**（2026-10-28） | 常规 52/52（StaticFileServletTest 6 用例 + Dump 全等/文件源/1MB 流式校验和/截断关连接）；实施要点：Dump 拆 DumpHead（字节序全等锁定）、fd 经 shared_ptr（拷贝安全最末引用关闭）、thread_local 64KB 缓冲分块 pread；实施修正：const 成员 realpath 经静态 helper 初始化列表化 |
| U6 AccessLog | **完成**（2026-10-28） | 常规 52/52（2 新用例：格式/默认关）；bytes 字段为头长约计（128+body/fileSize——精确头长需响应对象持 Dump 产物，登记观察项）；计时经 nowNs 起点/收尾 |
| U7 WS 钩子（弹性位） | **完成**（2026-10-28） | 常规 52/52（3 新用例：接管+leftover 透出/脱离超时/无 handler 501）；实施修正：leftover 偏移双重相加下溢（off 已累计 consumed）gdb 定位修正 |

**H2 收官（2026-10-28）**：U1–U7 全部落地（含弹性位）。新增 3 测试目标（HttpClientTest/SessionTest/StaticFileServletTest），52 测试目标常规全绿 + ASAN/TSAN 终验。H3 清单：TLS/gzip(zlib 裁决)/异步 Servlet/WS 帧层/TCP backlog(Q34)/Client 连接池+重定向/multipart/限流/X-Forwarded-For/通用流式。

## H3 登记清单

TLS（独立轮）/gzip（zlib 裁决）/异步 Servlet/WS 帧层/TCP backlog（Q34）/Client 连接池+重定向/
multipart/限流/X-Forwarded-For/通用流式（chunked 生成/BodyProvider/sendfile）

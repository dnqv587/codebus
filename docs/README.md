# codebus 文档索引

本目录存放项目文档：`spec/` 为设计文档与实施计划，根级为问题登记等活文档。

## 目录结构

```
docs/
├── README.md        # 本索引
├── known-issues.md  # 测试与已知缺陷登记（活文档）
└── spec/            # 设计文档与计划
    ├── 2026-10-19-unit-test-refactor-design.md
    ├── 2026-10-19-defect-fix-plan.md
    ├── 2026-10-22-tcp-server-design.md
    ├── 2026-10-23-tcp-client-design.md
    ├── 2026-10-24-udp-design.md
    ├── 2026-10-27-http-design.md
    ├── 2026-10-28-http-r2-design.md
    ├── 2026-10-28-http-r3-design.md
    └── 2026-11-05-http-r4-tls-design.md
```

## 文档清单

| 文档 | 说明 | 状态 |
|---|---|---|
| [测试与已知缺陷](known-issues.md) | D 编号缺陷登记（源码位置/用例/绕行/启用条件）、本轮新发现问题（D28+）、疑点清单（Q 系列）、空缺编号说明 | 活文档，随测试实施持续更新 |
| [单元测试体系重构设计](spec/2026-10-19-unit-test-refactor-design.md) | test/ 目录重组为 36 个单元测试目标的设计：单元划分裁决、六维度场景矩阵与 Gap 分析、网络模块高并发与内存安全排查方案（TSAN/ASAN）、CMake 与 helper 重组、缺陷迁移对照、分阶段实施计划 | **已实施完成**（2026-10-19，P-1–P12 全部落地；sanitizer 专项结果见 known-issues.md） |
| [HTTP 模块设计](spec/2026-10-27-http-design.md) | HTTP/1.1 Server（bus::http）：Java Servlet/Filter/Dispatcher 编程模型映射、分层架构（协议对象/解析适配/Servlet 容器/HttpServer 传输）、http_parser 封装状态机、可配置 worker 业务池与 pipeline 单飞、慢速攻击防护，V0–V6 分单元实施 | **已实施完成**（2026-10-27，V0–V6 全部落地；49 测试目标三矩阵全绿） |
| [缺陷逐项修复计划](spec/2026-10-19-defect-fix-plan.md) | 已登记缺陷 D 系列按「简单优先」逐项修复（P0–P26） | **已收官**（P0–P26 完成；P22/D8 用户裁决功能延后维持 DISABLED） |
| [TcpServer 完善设计](spec/2026-10-22-tcp-server-design.md) | TcpServer 全栈完善：停止时序/连接生命周期/背压/压测扩展（P0–P8） | **已实施完成**（2026-10-22，P0–P8 闭环） |
| [TCP Client 设计](spec/2026-10-23-tcp-client-design.md) | Connector/TcpClient：非阻塞连接、失败退避重连、统一回调通知（U0–U4） | **已实施完成**（2026-10-23，U0–U4 闭环） |
| [UDP 模块设计](spec/2026-10-24-udp-design.md) | UdpServer/UdpClient（同步模型：V0–W2） | 实施中（W3 用例矩阵扩展+终验收账待做） |
| [HTTP 二期设计（H2）](spec/2026-10-28-http-r2-design.md) | 防护加固、路由模式（路径参数）、异步 HttpClient、Session、流式最小形态+静态文件、AccessLog、WS 接管钩子（U1–U7） | **已实施完成**（2026-10-28，U1–U7 落地） |
| [HTTP 三期设计（H3）](spec/2026-10-28-http-r3-design.md) | 协议深度轮：TCP backlog、gzip、通用流式（sendfile/chunked/Range）、WebSocket 帧层、收尾批（U1–U5） | **已实施完成**（2026-10-30，U1–U5 落地；56 测试目标） |
| [HTTP 四期设计（H4·TLS）](spec/2026-11-05-http-r4-tls-design.md) | TLS 独立轮：OpenSSL/SslContext、SslSession 内存 BIO 状态机注入 TCPConnection、HttpsServer/HttpClient https、wss、WS sweep 优化（U0–U8） | **实施中**（2026-11-05 立项） |

## 约定

- 涉及代码改动的需求，设计文档统一放 `docs/spec/`，命名 `YYYY-MM-DD-<topic>-design.md`
- 测试发现的缺陷统一登记 [known-issues.md](known-issues.md)，D/Q 编号只增不复用；当前下一可用编号 **D55 / Q45**（以 known-issues.md 头部指针为准，随每轮登记更新；D12/D25/D26 永久空缺）
- 缺陷与测试代码的对应关系：`TODO(Dxx)` 注释 + `DISABLED_` 用例前缀，可用 `grep -rhoE "TODO\(D[0-9]+.*\)" test/` 核对

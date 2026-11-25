# codebus

C++20 网络框架库（One Loop Per Thread，参考 muduo/sylar）。本表记录框架独有概念的规范语言；通用编程概念不收。

## Language

**哨兵地址（无效地址）**：
Address 默认构造的零值，表示「无地址」，作为失败返回的统一哨兵；不是可通信地址，有效性经 `isValid()` 判定。
_Avoid_: 空地址、null 地址

**Over（终结信号）**：
阻塞队列的终结信号——消费方停止阻塞、取尽即止；生产方不被拒绝，终结后是否入队由调用方契约保证。
_Avoid_: 关闭队列、停止队列

**排空（drain）**：
停止后消费方继续处理队列残余直至取尽的停止形态；是否排空由各组件 stop 契约决定（线程池 stopAndDrain 保证、普通 stop 丢弃；异步日志 stop 恒排空）。
_Avoid_: 清空、flush
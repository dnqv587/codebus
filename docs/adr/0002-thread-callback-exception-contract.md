# 线程回调异常契约

任何线程回调（Thread 入口、ThreadPool 任务、ioLoop 回调）抛异常一律不 catch，std::terminate 杀整个进程——与 muduo 同款的项目级契约，以死亡测试锁定并文档化。备选「池内 catch-log-继续」被否决：与事件循环路径不一致，且可能掩盖缺陷。终止前留证据是基础设施级 fail-fast（LOG_FATAL+Abort，见 ADR-0001）的职责，不是通用 catch。
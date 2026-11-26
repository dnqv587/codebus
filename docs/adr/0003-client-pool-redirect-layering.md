# 客户端池与重定向分层

HttpClient 是单 host 单连接单在途单元（const m_host、CAS 拒重入、D54 handoff）。连接池做成管理 N 个 HttpClient 实例的调度层（单 host、空闲优先+FIFO、惰性创建、池层不重试），而非改造单实例状态机——单在途 CAS 与池并发是不同层，改内部会破坏 exactly-once/handoff 契约。重定向跟随放在 HttpClient 层、仅同 host（跨 host 需新实例，临时实例编排的所有权/生命周期成本过剩）；跨 host 3xx 原样交付调用方，登记残留。备选「池层编排跨 host 重定向」与「单实例内部多路复用」均被否决：前者无需求驱动且复杂，后者动摇既有契约面。
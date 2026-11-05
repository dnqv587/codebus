# 测试证书资产（H4 TLS 轮）

本目录是 H4 轮（U1–U7）TLS 测试的共用输入，**产物检入仓库**（测试不依赖构建机 openssl CLI，可复现；curl/libwebsockets 同法）。

| 文件 | 用途 |
|---|---|
| `ca.crt` / `ca.key` | 自签测试 CA（信任锚；U5 客户端 `verifyPeer=true` 用） |
| `server.crt` / `server.key` | CA 签发的服务端证书，CN=localhost，SAN=`DNS:localhost, IP:127.0.0.1`（R12：IP 条目必含，否则 127.0.0.1 回环连接 verifyPeer 失败） |
| `untrusted.crt` / `untrusted.key` | 自签非 CA 证书（同 SAN）；U5「不受信证书被拒」用例与 U1「证书私钥不配对」用例输入 |

## 到期与轮换

- 有效期 `-days 7300`，**2046-11 到期**——到期监控见 known-issues **Q44**
- 轮换：重跑 [`scripts/gen-test-certs.sh`](../../scripts/gen-test-certs.sh) 并同步检入全部产物

## 再生成

```bash
scripts/gen-test-certs.sh   # 内含 openssl verify 自检
```

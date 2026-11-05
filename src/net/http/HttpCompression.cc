//***********************************************
// name      : HttpCompression.cc
// brief     : HTTP gzip 压缩纯函数层实现
// attention : None
//***********************************************
#include "net/http/HttpCompression.h"

#include <zlib.h>

#include "log/Logger.h"
#include "net/http/HttpUtil.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("sys");

bool GzipCompress(std::string_view in, std::string& out, int level) noexcept {
    z_stream zs{};
    // 16+MAX_WBITS：产出带 gzip 头尾的 RFC 1952 帧（非裸 deflate 流）
    if (deflateInit2(&zs, level, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        LOG_WARN(g_logger) << "GzipCompress: deflateInit2 failed";
        return false;
    }
    out.clear();
    out.reserve(in.size() / 2 + 64);
    std::string input(in);
    zs.next_in = reinterpret_cast<Bytef*>(input.data());
    zs.avail_in = static_cast<uInt>(input.size());
    char buf[16_KB];
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, Z_FINISH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            LOG_WARN(g_logger) << "GzipCompress: deflate ret=" << ret;
            deflateEnd(&zs);
            return false;
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return true;
}

int GzipDecompress(std::string_view in, std::string& out, size_t maxOut) noexcept {
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {  // 自动识别 gzip 头
        LOG_WARN(g_logger) << "GzipDecompress: inflateInit2 failed";
        return -1;
    }
    out.clear();
    out.reserve(std::min(maxOut, in.size() * 4 + 64));
    std::string input(in);
    zs.next_in = reinterpret_cast<Bytef*>(input.data());
    zs.avail_in = static_cast<uInt>(input.size());
    char buf[16_KB];
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return -1;  // 非 gzip/截断/损坏
        }
        size_t got = sizeof(buf) - zs.avail_out;
        if (out.size() + got > maxOut) {
            inflateEnd(&zs);
            return -2;  // 超上限（zlib bomb 防护）
        }
        out.append(buf, got);
        if (ret == Z_BUF_ERROR && zs.avail_in == 0 && got == 0) {
            inflateEnd(&zs);
            return -1;  // 输入耗尽未到流尾（截断）
        }
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return 0;
}

bool IsCompressibleMime(std::string_view content_type) noexcept {
    if (content_type.empty()) {
        return false;
    }
    // MIME 类型不区分大小写；参数后缀（; charset=…）不影响前缀判定
    auto starts = [&](std::string_view p) {
        return content_type.size() >= p.size() && EqualCI(content_type.substr(0, p.size()), p);
    };
    return starts("text/") || starts("application/json") || starts("application/javascript") ||
           starts("application/xml") || starts("image/svg");
}

}  // namespace bus::http

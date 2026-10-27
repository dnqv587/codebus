//***********************************************
// name      : Buffer.hpp
// brief     : 缓冲区模块：Slice 只读字节视图 + Buffer 三区读写缓冲
// attention : Slice 不拥有数据生命周期；Buffer 扩容后旧 Slice 失效
//***********************************************
#pragma once
#include "copyable.hpp"
#include "Macro.h"
#include <cstddef>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <string_view>
#include <span>
#include <vector>
#include <memory>

namespace bus {

class Slice : copyable {
 public:
    using value_type = std::byte;

    constexpr Slice() = default;

    template <typename It>
    constexpr Slice(It data, size_t size) : m_data(data, size) {}

    template <typename It>
    constexpr Slice(It begin, It end) : m_data(begin, end) {}

    /// @brief 从 char* 构造字节视图
    /// @param data 字节数据首地址
    /// @param size 字节数
    Slice(char* data, size_t size) noexcept : m_data(reinterpret_cast<const std::byte*>(data), size) {}

    /// @brief 从 const char* 构造字节视图
    /// @param data 字节数据首地址
    /// @param size 字节数
    Slice(const char* data, size_t size) noexcept : m_data(reinterpret_cast<const std::byte*>(data), size) {}

    /// @brief 从 void* 构造字节视图
    /// @param data 字节数据首地址
    /// @param size 字节数
    Slice(void* data, size_t size) noexcept : m_data(reinterpret_cast<const std::byte*>(data), size) {}

    /// @brief 从 const void* 构造字节视图
    /// @param data 字节数据首地址
    /// @param size 字节数
    Slice(const void* data, size_t size) noexcept : m_data(reinterpret_cast<const std::byte*>(data), size) {}

    /// @brief 从 string_view 构造字节视图
    /// @param s 字符串视图
    Slice(std::string_view s) noexcept : m_data(reinterpret_cast<const std::byte*>(s.data()), s.size()) {}

    /// @brief 获取底层字节指针
    /// @return 只读字节指针
    constexpr const std::byte* data() const noexcept {
        return m_data.data();
    }

    /// @brief 获取内部字节视图
    /// @return 只读 span
    constexpr std::span<const std::byte> peek() const noexcept {
        return m_data;
    }

    constexpr size_t size() const noexcept {
        return m_data.size();
    }

    constexpr bool empty() const noexcept {
        return m_data.empty();
    }

    constexpr std::span<const std::byte>::iterator begin() const noexcept {
        return m_data.begin();
    }

    constexpr std::span<const std::byte>::iterator end() const noexcept {
        return m_data.end();
    }

    constexpr std::byte operator[](size_t n) const {
        assert(n < size());
        return m_data[n];
    }

    std::string_view toString() const noexcept {
        return std::string_view(reinterpret_cast<const char*>(data()), size());
    }

    friend std::ostream& operator<<(std::ostream& os, const Slice& value) {
        os << value.toString();
        return os;
    }

 private:
    /// @brief 字节数据只读视图
    std::span<const std::byte> m_data;
};

/// +-------------------+------------------+------------------+
/// | prependable bytes |  readable bytes  |  writable bytes  |
/// |                   |     (CONTENT)    |                  |
/// +-------------------+------------------+------------------+
/// |                   |                  |                  |
/// 0      <=      readerIndex   <=   writerIndex    <=     capacity

class Buffer : copyable {
 public:
    static constexpr size_t CheapPrependSize = 8;
    using Iter = std::vector<std::byte>::iterator;
    using ptr = std::shared_ptr<Buffer>;

    constexpr explicit Buffer(size_t capacity)
        : m_capacity(capacity + CheapPrependSize),
          m_buffer(capacity + CheapPrependSize),
          m_read_idx(CheapPrependSize),
          m_write_idx(CheapPrependSize) {
        assert(ReadableSize() == 0);
        assert(WritableSize() == capacity);
    }

    // constexpr size_t size() const noexcept{
    //     assert(m_write_idx > m_read_idx);
    //     return ReadableSize();
    // }

    constexpr Iter begin() noexcept {
        return m_buffer.begin();
    }

    constexpr Iter end() noexcept {
        return m_buffer.end();
    }

    constexpr Iter ReadBegin() noexcept {
        return m_buffer.begin() + m_read_idx;
    }

    constexpr Iter WriteBegin() noexcept {
        return m_buffer.begin() + m_write_idx;
    }

    constexpr size_t WritableSize() const noexcept {
        assert(m_capacity >= m_write_idx);
        return m_capacity - m_write_idx;
    }

    constexpr size_t ReadableSize() const noexcept {
        assert(m_write_idx >= m_read_idx);
        return m_write_idx - m_read_idx;
    }
    /// @brief 跳过n字节read index
    /// @param n
    constexpr void SkipRead(size_t n) noexcept {
        assert(m_read_idx + n <= m_write_idx);
        m_read_idx += n;
    }
    /// @brief 跳过n字节write index
    /// @param n
    constexpr void SkipWrite(size_t n) noexcept {
        assert(m_write_idx + n <= m_capacity);
        m_write_idx += n;
    }

    constexpr size_t PrependableSize() const noexcept {
        return m_read_idx;
    }

    constexpr size_t Capacity() const noexcept {
        return m_capacity;
    }

    /// @brief 截断read_idx + n后的数据，若n为0则清空所有数据
    /// @param n 长度
    constexpr void Truncate(size_t n) noexcept {
        if (n == 0) {
            m_read_idx = CheapPrependSize;
            m_write_idx = CheapPrependSize;
        } else {
            m_write_idx = m_read_idx + n;
        }
    }

    constexpr void Reset() noexcept {
        Truncate(0);
    }

    constexpr void Reserve(size_t len) noexcept {
        if (m_capacity > len + CheapPrependSize) {
            return;
        }
        m_buffer.resize(len + CheapPrependSize);
        m_capacity = len + CheapPrependSize;
    }

    constexpr void EnsureWriteableSize(size_t len) noexcept {
        if (UNLIKELY(WritableSize() < len)) {
            grow(len);
        }
        assert(WritableSize() >= len);
    }

    constexpr void Write(Slice slice) noexcept {
        EnsureWriteableSize(slice.size());
        std::copy(slice.begin(), slice.end(), m_buffer.begin() + m_write_idx);
        assert(m_write_idx + slice.size() <= m_capacity);
        m_write_idx += slice.size();
    }

    constexpr void Prepend(Slice slice) noexcept {
        assert(slice.size() <= PrependableSize());
        m_read_idx -= slice.size();
        std::copy(slice.begin(), slice.end(), m_buffer.begin() + m_read_idx);
    }

    constexpr Slice Read(size_t len) noexcept {
        if (LIKELY(len <= ReadableSize())) {
            Slice slice(m_buffer.begin() + m_read_idx, len);
            m_read_idx += len;
            return slice;
        }
        return ReadAll();
    }

    constexpr Slice ReadAll() noexcept {
        Slice slice(m_buffer.begin() + m_read_idx, ReadableSize());
        Reset();
        return slice;
    }

    constexpr Slice Peek(size_t len) noexcept {
        if (LIKELY(len <= ReadableSize())) {
            return {m_buffer.begin() + m_read_idx, len};
        }
        return PeekAll();
    }

    constexpr Slice PeekAll() noexcept {
        return {m_buffer.begin() + m_read_idx, ReadableSize()};
    }

    constexpr Iter FindEOL() noexcept {
        auto eol = std::find(m_buffer.begin() + m_read_idx, m_buffer.begin() + m_write_idx, std::byte{'\n'});
        return eol == m_buffer.end() ? m_buffer.end() : eol;
    }

 private:
    void grow(size_t len) {
        if (WritableSize() + PrependableSize() < len + CheapPrependSize)  // 可写容量不足
        {
            size_t grow_size = 2 * m_capacity + len;
            m_buffer.resize(grow_size);
            m_capacity = grow_size;
            assert(WritableSize() >= len);
        } else  // 进行滑动窗口
        {
            assert(CheapPrependSize < m_read_idx);
            size_t readable = ReadableSize();
            std::move(m_buffer.begin() + m_read_idx, m_buffer.begin() + m_write_idx,
                      m_buffer.begin() + CheapPrependSize);
            m_read_idx = CheapPrependSize;
            m_write_idx = m_read_idx + readable;
            assert(readable == ReadableSize());
        }
    }

 private:
    size_t m_capacity;
    std::vector<std::byte> m_buffer;
    size_t m_read_idx;
    size_t m_write_idx;
};

}  // namespace bus
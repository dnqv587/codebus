#include "TCPConnection.h"
#include "Channel.h"
#include "log/Logger.h"
#include "base/Macro.h"
#include "Socket.h"
#include "net/EventLoop.h"

using namespace bus;

Logger&  g_log = GET_LOGGER("sys");

TCPConnection::TCPConnection(View<EventLoop> loop, std::string_view name, socket_t sock, const Address& localAddr, const Address& remoteAddr)
:m_loop(loop.NativeHandle()),
m_name(name),
m_sock(std::make_unique<Socket>(sock)),
m_local_addr(localAddr),
m_remote_addr(remoteAddr),
m_status(TCPConnection::Disconnected),
m_channel(std::make_unique<Channel>(loop,sock)),
m_input_buffer(std::make_unique<Buffer>(1024)),
m_output_buffer(std::make_unique<Buffer>(1024))
{
    m_channel->setReadCallback([this](){
        handleRead();
    });
    m_channel->setWriteCallback([this](){
        handleWrite();
    });
    m_channel->setErrorCallback([this](){
        handleError();
    });

    m_sock->setKeepAlive(true);
    LOG_INFO(g_log)<<"TCPConnection ["<<m_name<<"] " << " fd="<<m_sock->fd()<< ", local addr=" << m_local_addr.toString()<<
                                     ", peer Addr" << m_remote_addr.toString()<<" status:"<<StatusEnumToString(m_status);
}

TCPConnection::~TCPConnection(){
    LOG_INFO(g_log)<<"~TCPConnection ["<<m_name<<"] at"<< this<<" fd="<<m_sock->fd()<<" status"<<StatusEnumToString(m_status);
    assert(m_status == TCPConnection::Disconnected);
}

std::string_view TCPConnection::StatusEnumToString(TCPConnection::Status status) const noexcept{
        CASE_STRING_BEGIN(status);
        CASE_STRING(Disconnected);
        CASE_STRING(Connecting);
        CASE_STRING(Connected);
        CASE_STRING(Disconnecting);
        CASE_STRING_END();
}

void TCPConnection::handleRead(){
    assert(m_loop->isInLoopThread());
    ssize_t n = m_sock->read(m_input_buffer.get());
    if(n > 0){
        std::invoke(m_msg_cb,shared_from_this(),m_input_buffer.get());
    } else if(n == 0){
        handleClose();
    } else {
        int err = errno;
        if(err==EAGAIN || err==EWOULDBLOCK || err==EINTR){
            LOG_ERROR(g_log)<< "this="<<this<<" TCPConnection::handleRead errno="<<err<<" "<<strerror(err);
        } else {
            handleError();
        }
        
    }
}

void TCPConnection::handleWrite(){
    assert(m_loop->isInLoopThread());
    Slice buf = m_output_buffer->PeekAll();
    ssize_t n = m_sock->write(buf.data(),buf.size());
    if(n > 0){
        m_output_buffer->SkipRead(n);
        if(m_output_buffer->ReadableSize() == 0){
            m_channel->DisableWriteEvent();
            if(m_write_cb){
                m_loop->RunInLoop([conn = shared_from_this()](){
                    std::invoke(conn->m_write_cb,conn);
                });
            }
        }
    } else {
        int err = errno;
        if(err == EAGAIN || err == EWOULDBLOCK || err==EINTR){
            LOG_ERROR(g_log)<< "this="<<this<<" TCPConnection::handleWrite errno="<<err<<" "<<strerror(err);
        } else {
            handleError();
        }
        
    }
}

void TCPConnection::handleError(){
    assert(m_loop->isInLoopThread());
    int err = m_sock->getSocketErrno();
    LOG_ERROR(g_log) << "handleError ["<< m_name <<"] errno="<<err<<" "<<strerror(err);
    m_status = Disconnecting;
    handleClose();
}
                  
void TCPConnection::handleClose(){
    assert(m_loop->isInLoopThread());
    LOG_INFO(g_log)<<"handle close ["<<m_name<<"] " << " fd="<<m_sock->fd()<< ", local addr=" << m_local_addr.toString()<<
                                     ", peer Addr" << m_remote_addr.toString()<<" status:"<<StatusEnumToString(m_status);
    if(m_status == Disconnected){
        return;
    }
    assert(m_status == Disconnecting);
    m_channel->DisableAllEvent();

    if(m_close_cb){
        std::invoke(m_close_cb,shared_from_this());
    }
    LOG_INFO(g_log)<<"handle close ["<<m_name<<"] " << " fd="<<m_sock->fd()<< ", local addr=" << m_local_addr.toString()<<
                                     ", peer Addr" << m_remote_addr.toString()<<" status:"<<StatusEnumToString(m_status);
    m_status = Disconnected;
}

void TCPConnection::close(){
    LOG_INFO(g_log)<<"close ["<<m_name<<"] " << " fd="<<m_sock->fd()<< ", local addr=" << m_local_addr.toString()<<
                                     ", peer Addr" << m_remote_addr.toString()<<" status:"<<StatusEnumToString(m_status);
    m_status = Disconnecting;
    m_loop->RunInLoop([conn = shared_from_this()](){
        assert(conn->m_loop->isInLoopThread());
        conn->handleClose();
    });
}

void TCPConnection::send(const std::string_view s){
    Slice data(s);
    send(data);
}

void TCPConnection::send(const void* data,size_t len){
    Slice s(data, len);
    send(s);
}

void TCPConnection::send(const Slice& msg){
    if(m_status != Connected){
        return;
    }
    m_loop->RunInLoop([conn=shared_from_this(),&msg](){
        conn->sendInLoop(msg);
    });
}

void TCPConnection::send(Buffer* buf){

}

void TCPConnection::sendInLoop(Slice data){
    assert(m_loop->isInLoopThread());
    if (m_status == Disconnected)
    {
        LOG_WARN(g_log) << "disconnected, give up writing";
        return;
    }
    ssize_t written = 0;
    size_t remain = data.size();
    //channel未监听写事件，且output buffer无数据。直接写
    if(!m_channel->isListeningWrite() && m_output_buffer->ReadableSize() == 0){
        written = m_sock->write(data.data(),data.size());
        if(written >=0 ){
            assert(data.size() >= written);
            remain = data.size() - written;
            if(remain==0 && m_write_cb){
                m_loop->RunInLoop([conn = shared_from_this()](){
                    std::invoke(conn->m_write_cb,conn);
                });
            }

        } else {
            int err = errno;
            written = 0;
            if(!(err == EAGAIN || err == EWOULDBLOCK || err==EINTR)){
                LOG_ERROR(g_log)<<"sendInLoop write faild errno="<<err<<" "<<strerror(err);
            }
            if(err == EPIPE || err == ECONNRESET){
                handleError();
            }
        }
    }
    assert(remain <= data.size());
    if (remain > 0) {
      size_t old_len = m_output_buffer->ReadableSize();
      // 检查高水位
      if (old_len + remain >= m_hight_water_mark 
          && old_len < m_hight_water_mark 
          && m_hight_water_cb) {
        m_loop->RunInLoop([conn=shared_from_this(),len=old_len + remain](){
            conn->m_hight_water_cb(conn,len);
        });
      }
      //写入到out buffer
      Slice remain_data(data.begin()+written,remain);
      m_output_buffer->Write(remain_data);
      if(!m_channel->isListeningWrite()){
        m_channel->EnableWriteEvent();
      }
    }
}

socket_t TCPConnection::fd() const noexcept{
    return m_sock->fd();
}

void TCPConnection::setTcpNoDelay(bool on){
    m_sock->setTcpNoDelay(on);
}
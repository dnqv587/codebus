#include "LogAppender.h"
#include <assert.h>
#include <string.h>

File::File(const std::string &filename)
:m_file(::fopen(filename.c_str(),"ae"),[](FILE* file)->void{
    ::fclose(file);
}),
m_writtenBytes(0)
{
    assert(m_file);
    ::setbuffer(m_file.get(),m_buf,File::BUFFSIZE);
}

void File::Write(const char *log, size_t len)
{
    size_t written=0;
    size_t remain=len;
    while(remain)
    {
        size_t n=this->WriteUnlock(log,len);
        if(n !=remain)
        {
            int errnum=::ferror_unlocked(m_file.get());
            if(errnum)
            {
                ::fprintf(stderr,"LogFile::File::append error:%s\n",::strerror(errnum));
            }
        }
        written +=n;
        remain =len-written;
    }
    m_writtenBytes+=written;
}

void File::Flush() const
{
    if(0!=::fflush(m_file.get()))
    {
        ::perror("LogFile::File::flush error");
    }
}

size_t File::WriteUnlock(const char *log, size_t len)
{
    ::fwrite_unlocked(log,1,len,m_file.get());
}

#include <exception>
#include <errno.h>
#include <string.h>
#include "conn.h"
#include "log.h"
#include "fdwrapper.h"

conn::conn() //构造函数，初始化两个缓存，同时使用reset将状态全部清空
{
    m_srvfd = -1;
    m_clt_buf = new char[ BUF_SIZE ];
    if( !m_clt_buf )
    {
        throw std::exception();
    }
    m_srv_buf = new char[ BUF_SIZE ];
    if( !m_srv_buf )
    {
        throw std::exception();
    }
    reset();
}

conn::~conn()
{
    delete [] m_clt_buf;
    delete [] m_srv_buf;
}

void conn::init_clt( int sockfd, const sockaddr_in& client_addr ) //记录客户端
{
    m_cltfd = sockfd;
    m_clt_address = client_addr;
}

void conn::init_srv( int sockfd, const sockaddr_in& server_addr )
{
    m_srvfd = sockfd;
    m_srv_address = server_addr;
}

void conn::reset() //将状态全部清空
{
    m_clt_read_idx = 0;
    m_clt_write_idx = 0;
    m_srv_read_idx = 0;
    m_srv_write_idx = 0;
    m_srv_closed = false;
    m_cltfd = -1;
    memset( m_clt_buf, '\0', BUF_SIZE );
    memset( m_srv_buf, '\0', BUF_SIZE );
}

RET_CODE conn::read_clt() //客户端准备从缓冲区读
{
    int bytes_read = 0;
    while( true )
    {
        if( m_clt_read_idx >= BUF_SIZE ) //从客户端来的数据已经充满缓冲区，试图发送给服务器
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "the client read buffer is full, let server write" );
            return BUFFER_FULL;
        }

        bytes_read = recv( m_cltfd, m_clt_buf + m_clt_read_idx, BUF_SIZE - m_clt_read_idx, 0 ); //缓冲区未满接着从客户端读读
        if ( bytes_read == -1 ) //IO错误
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return IOERR;
        }
        else if ( bytes_read == 0 ) //关闭
        {
            return CLOSED;
        }

        m_clt_read_idx += bytes_read;
    }
    return ( ( m_clt_read_idx - m_clt_write_idx ) > 0 ) ? OK : NOTHING; //发送给服务器的数据理应小于从客户端读取数据
}

RET_CODE conn::write_clt() //服务器缓冲区准备发送给客户端数据
{
    int bytes_write = 0;
    while( true )
    {
        if( m_srv_read_idx <= m_srv_write_idx ) //服务器已经发送的数据多余可以发送的数据所以应当把缓冲区清空
        {
            m_srv_read_idx = 0;
            m_srv_write_idx = 0;
            return BUFFER_EMPTY;
        }
        //conn通过服务器缓冲区正常发送数据给客户端fd
        bytes_write = send( m_cltfd, m_srv_buf + m_srv_write_idx, m_srv_read_idx - m_srv_write_idx, 0 );
        if ( bytes_write == -1 )
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                return TRY_AGAIN;
            }
            log( LOG_ERR, __FILE__, __LINE__, "write client socket failed, %s", strerror( errno ) );
            return IOERR;
        }
        else if ( bytes_write == 0 )
        {
            return CLOSED;
        }

        m_srv_write_idx += bytes_write;
    }
}

RET_CODE conn::read_srv()
{
    int bytes_read = 0;
    while( true )
    {
        if( m_srv_read_idx >= BUF_SIZE )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "the server read buffer is full, let client write" );
            return BUFFER_FULL;
        }

        bytes_read = recv( m_srvfd, m_srv_buf + m_srv_read_idx, BUF_SIZE - m_srv_read_idx, 0 );
        if ( bytes_read == -1 )
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return IOERR;
        }
        else if ( bytes_read == 0 )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "the server should not close the persist connection" );
            return CLOSED;
        }

        m_srv_read_idx += bytes_read;
    }
    return ( ( m_srv_read_idx - m_srv_write_idx ) > 0 ) ? OK : NOTHING;
}

RET_CODE conn::write_srv()
{
    int bytes_write = 0;
    while( true )
    {
        if( m_clt_read_idx <= m_clt_write_idx )
        {
            m_clt_read_idx = 0;
            m_clt_write_idx = 0;
            return BUFFER_EMPTY;
        }

        bytes_write = send( m_srvfd, m_clt_buf + m_clt_write_idx, m_clt_read_idx - m_clt_write_idx, 0 );
        if ( bytes_write == -1 )
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                return TRY_AGAIN;
            }
            log( LOG_ERR, __FILE__, __LINE__, "write server socket failed, %s", strerror( errno ) );
            return IOERR;
        }
        else if ( bytes_write == 0 )
        {
            return CLOSED;
        }

        m_clt_write_idx += bytes_write;
    }
}



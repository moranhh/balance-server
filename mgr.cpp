#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <exception>
#include "log.h"
#include "mgr.h"

using std::pair;

int mgr::m_epollfd = -1;
int mgr::conn2srv( const sockaddr_in& address ) //和远端服务器进行连接
{
    int sockfd = socket( PF_INET, SOCK_STREAM, 0 );
    if( sockfd < 0 )
    {
        return -1;
    }

    if ( connect( sockfd, ( struct sockaddr* )&address, sizeof( address ) ) != 0  )
    {
        close( sockfd );
        return -1;
    }
    return sockfd;
}

mgr::mgr( int epollfd, const host& srv ) : m_logic_srv( srv )
{
    m_epollfd = epollfd;
    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, srv.m_hostname, &address.sin_addr );
    address.sin_port = htons( srv.m_port );
    log( LOG_INFO, __FILE__, __LINE__, "logcial srv host info: (%s, %d)", srv.m_hostname, srv.m_port );

    for( int i = 0; i < srv.m_conncnt; ++i ) //小于远端连接允许接受的连接数
    {
        sleep( 1 );
        int sockfd = conn2srv( address ); //尝试连接远端服务器
        if( sockfd < 0 ) //失败
        {
            log( LOG_ERR, __FILE__, __LINE__, "build connection %d failed", i );
        }
        else //成功 建立新连接
        {
            log( LOG_INFO, __FILE__, __LINE__, "build connection %d to server success", i );
            conn* tmp = NULL;
            try
            {
                tmp = new conn;
            }
            catch( ... )
            {
                close( sockfd );
                continue;
            }
            tmp->init_srv( sockfd, address ); //初始化新的conn
            m_conns.insert( pair< int, conn* >( sockfd, tmp ) ); 
        }
    }
}

mgr::~mgr()
{
}

int mgr::get_used_conn_cnt() //返回本manager负责的客户端连接和服务器连接总数
{
    return m_used.size();
}

conn* mgr::pick_conn( int cltfd  ) //从和服务器连接的conn中挑选一个出来给客户端
{
    if( m_conns.empty() ) //没有和远程服务器连接的conn可用
    {
        log( LOG_ERR, __FILE__, __LINE__, "%s", "not enough srv connections to server" );
        return NULL;
    }

    map< int, conn* >::iterator iter =  m_conns.begin();
    int srvfd = iter->first;
    conn* tmp = iter->second;
    if( !tmp )
    {
        log( LOG_ERR, __FILE__, __LINE__, "%s", "empty server connection object" );
        return NULL;
    }
    m_conns.erase( iter ); //将该conn从m_conn移到已经使用的conn(m_used)中
    m_used.insert( pair< int, conn* >( cltfd, tmp ) ); //并且该服务器连接conn对应的客户端conn也需要插入到m_used中
    m_used.insert( pair< int, conn* >( srvfd, tmp ) ); 
    add_read_fd( m_epollfd, cltfd );
    add_read_fd( m_epollfd, srvfd );
    log( LOG_INFO, __FILE__, __LINE__, "bind client sock %d with server sock %d", cltfd, srvfd );
    return tmp;
}

void mgr::free_conn( conn* connection ) //客户端断开连接，需要回收与之相连的服务器连接
{
    int cltfd = connection->m_cltfd;
    int srvfd = connection->m_srvfd;
    closefd( m_epollfd, cltfd );
    closefd( m_epollfd, srvfd );
    m_used.erase( cltfd );
    m_used.erase( srvfd );
    connection->reset(); //将该连接置空
    m_freed.insert( pair< int, conn* >( srvfd, connection ) );
}

void mgr::recycle_conns() //客户端每次断开连接都将使一个服务器连接被回收，这里重新利用被回收的服务器连接
{
    if( m_freed.empty() ) //没有被回收的服务器连接
    {
        return;
    }
    for( map< int, conn* >::iterator iter = m_freed.begin(); iter != m_freed.end(); iter++ )
    {
        sleep( 1 );
        int srvfd = iter->first;
        conn* tmp = iter->second;
        srvfd = conn2srv( tmp->m_srv_address ); //重新尝试连接服务器
        if( srvfd < 0 )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "fix connection failed");
        }
        else
        {
            log( LOG_INFO, __FILE__, __LINE__, "%s", "fix connection success" );
            //重新初始化连接并且插入到可用连接map中
            tmp->init_srv( srvfd, tmp->m_srv_address ); 
            m_conns.insert( pair< int, conn* >( srvfd, tmp ) );
        }
    }
    m_freed.clear();
}

//具体的客户端处理函数
RET_CODE mgr::process( int fd, OP_TYPE type )
{
    conn* connection = m_used[ fd ];
    if( !connection )
    {
        return NOTHING;
    }
    if( connection->m_cltfd == fd )
    {
        int srvfd = connection->m_srvfd;
        switch( type )
        {
            case READ: //缓冲区准备从客户端读
            {
                RET_CODE res = connection->read_clt();
                switch( res )
                {
                    case OK:
                    {
                        log( LOG_DEBUG, __FILE__, __LINE__, "content read from client: %s", connection->m_clt_buf );
                    }
                    case BUFFER_FULL:
                    {
                        modfd( m_epollfd, srvfd, EPOLLOUT ); //客户端缓冲区满，conn试图向服务器写，监听服务器fd可写事件
                        break;
                    }
                    case IOERR:
                    case CLOSED:
                    {
                        free_conn( connection );
                        return CLOSED;
                    }
                    default:
                        break;
                }
                if( connection->m_srv_closed )//服务器关闭，回收该连接
                {
                    free_conn( connection );
                    return CLOSED;
                }
                break;
            }
            case WRITE: //conn试图从服务器缓冲区给客户端fd发送数据
            {
                RET_CODE res = connection->write_clt();
                switch( res )
                {
                    case TRY_AGAIN:
                    {
                        modfd( m_epollfd, fd, EPOLLOUT ); //给客户端fd添加写的通知
                        break;
                    }
                    case BUFFER_EMPTY: //服务器缓冲区为空，暂时还不能给客户端fd发送数据
                    {
                        modfd( m_epollfd, srvfd, EPOLLIN );
                        modfd( m_epollfd, fd, EPOLLIN );
                        break;
                    }
                    case IOERR:
                    case CLOSED:
                    {
                        free_conn( connection );
                        return CLOSED;
                    }
                    default:
                        break;
                }
                if( connection->m_srv_closed )
                {
                    free_conn( connection );
                    return CLOSED;
                }
                break;
            }
            default:
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "other operation not support yet" );
                break;
            }
        }
    }
    else if( connection->m_srvfd == fd )
    {
        int cltfd = connection->m_cltfd;
        switch( type )
        {
            case READ:
            {
                RET_CODE res = connection->read_srv();
                switch( res )
                {
                    case OK:
                    {
                        log( LOG_DEBUG, __FILE__, __LINE__, "content read from server: %s", connection->m_srv_buf );
                    }
                    case BUFFER_FULL:
                    {
                        modfd( m_epollfd, cltfd, EPOLLOUT );
                        break;
                    }
                    case IOERR:
                    case CLOSED:
                    {
                        modfd( m_epollfd, cltfd, EPOLLOUT );
                        connection->m_srv_closed = true;
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            case WRITE:
            {
                RET_CODE res = connection->write_srv();
                switch( res )
                {
                    case TRY_AGAIN:
                    {
                        modfd( m_epollfd, fd, EPOLLOUT );
                        break;
                    }
                    case BUFFER_EMPTY:
                    {
                        modfd( m_epollfd, cltfd, EPOLLIN );
                        modfd( m_epollfd, fd, EPOLLIN );
                        break;
                    }
                    case IOERR:
                    case CLOSED:
                    {
                        modfd( m_epollfd, cltfd, EPOLLOUT );
                        connection->m_srv_closed = true;
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            default:
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "other operation not support yet" );
                break;
            }
        }
    }
    else
    {
        return NOTHING;
    }
    return OK;
}

//初始化连接时是按顺序初始化给定个数的连接，比如5个srv连接，就会让前1个进程连接5个远端服务器,因为设置中一个进程负责5个远端服务器
//均衡负载针对的是客户端，从已经连接的进程中挑选负担最少的提供给客户端连接
#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H

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
#include <vector>
#include "log.h"
#include "fdwrapper.h"

using std::vector;

class process
{
public:
    process() : m_pid( -1 ){}

public:
    int m_busy_ratio; //描述进程的忙碌程度，后面做均衡负载用
    pid_t m_pid;
    int m_pipefd[2]; //和父进程通信管道，子进程使用1端
};

template< typename C, typename H, typename M > //C 具体的连接 H host M manager
class processpool
{
private:
    processpool( int listenfd, int process_number = 8 );
public:
    static processpool< C, H, M >* create( int listenfd, int process_number = 8 )
    {
        if( !m_instance )
        {
            m_instance = new processpool< C, H, M >( listenfd, process_number ); //单例模式
        }
        return m_instance;
    }
    ~processpool()
    {
        delete [] m_sub_process;
    }
    void run( const vector<H>& arg );

private:
    void notify_parent_busy_ratio( int pipefd, M* manager );
    int get_most_free_srv();
    void setup_sig_pipe();
    void run_parent();
    void run_child( const vector<H>& arg );

private:
    static const int MAX_PROCESS_NUMBER = 16; 
    static const int USER_PER_PROCESS = 65536;
    static const int MAX_EVENT_NUMBER = 10000;
    int m_process_number;
    int m_idx; //进程id 父进程为-1
    int m_epollfd; //每个进程各自的epollfd
    int m_listenfd; //监听fd
    int m_stop;
    process* m_sub_process; //子进程数组，通过m_idx索引
    static processpool< C, H, M >* m_instance;
};

template< typename C, typename H, typename M >
processpool< C, H, M >* processpool< C, H, M >::m_instance = NULL;

static int EPOLL_WAIT_TIME = 5000; //epoll_wait 超时时长
static int sig_pipefd[2]; //信号管道

static void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( sig_pipefd[1], ( char* )&msg, 1, 0 ); //信号写入管道以通知主循环
    errno = save_errno;
}

static void addsig( int sig, void( *handler )(int), bool restart = true ) //设置信号处理函数
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART; //信号中断结束后继续之前被中断的程序
    }
    sigfillset( &sa.sa_mask ); //掩码全部填上，信号处理过程中不再接受任何信号
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

template< typename C, typename H, typename M >
processpool< C, H, M >::processpool( int listenfd, int process_number ) 
    : m_listenfd( listenfd ), m_process_number( process_number ), m_idx( -1 ), m_stop( false )
{
    assert( ( process_number > 0 ) && ( process_number <= MAX_PROCESS_NUMBER ) );

    m_sub_process = new process[ process_number ];
    assert( m_sub_process );

    for( int i = 0; i < process_number; ++i ) //建立process_number个子进程，
    {
        int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd ); //设置和父进程间的管道
        assert( ret == 0 );

        m_sub_process[i].m_pid = fork(); //建立子进程
        assert( m_sub_process[i].m_pid >= 0 );
        if( m_sub_process[i].m_pid > 0 ) //父进程中使用管道0端
        {
            close( m_sub_process[i].m_pipefd[1] );
            m_sub_process[i].m_busy_ratio = 0; //父进程不参与均衡负载的选择中，忙碌程度为0
            continue;
        }
        else //子进程中使用管道1端
        {
            close( m_sub_process[i].m_pipefd[0] );
            m_idx = i; //子进程序号
            break;
        }
    }
}

template< typename C, typename H, typename M >
int processpool< C, H, M >::get_most_free_srv() //获取最空闲的子进程
{
    int ratio = m_sub_process[0].m_busy_ratio;
    int idx = 0;
    for( int i = 0; i < m_process_number; ++i )
    {
        if( m_sub_process[i].m_busy_ratio < ratio )
        {
            idx = i;
            ratio = m_sub_process[i].m_busy_ratio;
        }
    }
    return idx;
}

template< typename C, typename H, typename M >
void processpool< C, H, M >::setup_sig_pipe() //设置信号管道
{
    m_epollfd = epoll_create( 5 );
    assert( m_epollfd != -1 );

    int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, sig_pipefd );
    assert( ret != -1 );

    setnonblocking( sig_pipefd[1] ); //1端非阻塞写，系统发送信号
    add_read_fd( m_epollfd, sig_pipefd[0] ); //0端监听读，添加到epoll内核中

    addsig( SIGCHLD, sig_handler );
    addsig( SIGTERM, sig_handler );
    addsig( SIGINT, sig_handler );
    addsig( SIGPIPE, SIG_IGN );
}

template< typename C, typename H, typename M >
void processpool< C, H, M >::run( const vector<H>& arg )
{
    if( m_idx != -1 ) //根据m_idx判断运行父进程还是子进程
    {
        run_child( arg );
        return;
    }
    run_parent();
}

template< typename C, typename H, typename M >
void processpool< C, H, M >::notify_parent_busy_ratio( int pipefd, M* manager ) //通知父进程该子进程目前连接的个数
{
    int msg = manager->get_used_conn_cnt();
    send( pipefd, ( char* )&msg, 1, 0 );    
}

template< typename C, typename H, typename M >
void processpool< C, H, M >::run_child( const vector<H>& arg )
{
    setup_sig_pipe();

    int pipefd_read = m_sub_process[m_idx].m_pipefd[ 1 ];
    add_read_fd( m_epollfd, pipefd_read );  //父子进程都只需要监听可读事件

    epoll_event events[ MAX_EVENT_NUMBER ];

    M* manager = new M( m_epollfd, arg[m_idx] );
    assert( manager );

    int number = 0;
    int ret = -1;

    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "epoll failure" ); //epoll_wait失败将错误写入日志
            break;
        }

        if( number == 0 )//暂无事件就绪，可能是所有服务器连接都被回收，所以重新使用这些服务器连接
        {
            manager->recycle_conns(); 
            continue;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( ( sockfd == pipefd_read ) && ( events[i].events & EPOLLIN ) ) //父进程交来的信息，说明有连接到来
            {
                int client = 0;
                ret = recv( sockfd, ( char* )&client, sizeof( client ), 0 );
                if( ( ( ret < 0 ) && ( errno != EAGAIN ) ) || ret == 0 ) 
                {
                    continue;
                }
                else
                {
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof( client_address );
                    int connfd = accept( m_listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                    if ( connfd < 0 )
                    {
                        log( LOG_ERR, __FILE__, __LINE__, "errno: %s", strerror( errno ) );
                        continue;
                    }
                    add_read_fd( m_epollfd, connfd );
                    C* conn = manager->pick_conn( connfd );
                    if( !conn )
                    {
                        closefd( m_epollfd, connfd );
                        continue;
                    }
                    conn->init_clt( connfd, client_address );       //客户端连接初始化
                    notify_parent_busy_ratio( pipefd_read, manager ); //通知父进程该子进程连接的个数
                }
            }
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) ) //处理信号
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 );
                if( ret <= 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )
                                {
                                    continue;
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                m_stop = true;
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN ) //客户端发来的请求，有消息要读
            {
                 RET_CODE result = manager->process( sockfd, READ ); //处理客户端的请求
                 switch( result )
                 {
                     case CLOSED: //客户端关闭
                     {
                         notify_parent_busy_ratio( pipefd_read, manager );
                         break;
                     }
                     default:
                         break;
                 }
            }
            else if( events[i].events & EPOLLOUT ) //客户端发来的请求
            {
                 RET_CODE result = manager->process( sockfd, WRITE );
                 switch( result )
                 {
                     case CLOSED:
                     {
                         notify_parent_busy_ratio( pipefd_read, manager );
                         break;
                     }
                     default:
                         break;
                 }
            }
            else
            {
                continue;
            }
        }
    }

    close( pipefd_read );
    close( m_epollfd );
}

template< typename C, typename H, typename M >
void processpool< C, H, M >::run_parent()
{
    setup_sig_pipe();

    for( int i = 0; i < m_process_number; ++i )
    {
        add_read_fd( m_epollfd, m_sub_process[i].m_pipefd[ 0 ] ); //和每一个子进程通信的管道都需要注册读事件
    }

    add_read_fd( m_epollfd, m_listenfd ); //监听连接的fd

    epoll_event events[ MAX_EVENT_NUMBER ];
    int sub_process_counter = 0;
    int new_conn = 1;
    int number = 0;
    int ret = -1;

    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "epoll failure" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( sockfd == m_listenfd ) //监听到新连接
            {
                int idx = get_most_free_srv(); //找到最空闲的子进程
                send( m_sub_process[idx].m_pipefd[0], ( char* )&new_conn, sizeof( new_conn ), 0 );
                log( LOG_INFO, __FILE__, __LINE__, "send request to child %d", idx ); //写入日志，级别是通知
            }
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) ) //处理信号
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 );
                if( ret <= 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            case SIGCHLD: //子进程停止
                            {
                                pid_t pid;
                                int stat;
                                while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 ) //关闭停止的子进程
                                {
                                    for( int i = 0; i < m_process_number; ++i )
                                    {
                                        if( m_sub_process[i].m_pid == pid )
                                        {
                                            log( LOG_INFO, __FILE__, __LINE__, "child %d join", i );
                                            close( m_sub_process[i].m_pipefd[0] ); //关闭对应管道
                                            m_sub_process[i].m_pid = -1; //pid置-1
                                        }
                                    }
                                }
                                m_stop = true;
                                for( int i = 0; i < m_process_number; ++i ) //所有子进程停止则停止服务器
                                {
                                    if( m_sub_process[i].m_pid != -1 )
                                    {
                                        m_stop = false;
                                    }
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                log( LOG_INFO, __FILE__, __LINE__, "%s", "kill all the clild now" );
                                for( int i = 0; i < m_process_number; ++i )
                                {
                                    int pid = m_sub_process[i].m_pid;
                                    if( pid != -1 )
                                    {
                                        kill( pid, SIGTERM ); //发送SIGTERM信号给子进程，等待子进程停止
                                    }
                                }
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN ) //接受子进程建立连接后发来的该子进程的busy程度
            {
                int busy_ratio = 0;
                ret = recv( sockfd, ( char* )&busy_ratio, sizeof( busy_ratio ), 0 );
                if( ( ( ret < 0 ) && ( errno != EAGAIN ) ) || ret == 0 )
                {
                    continue;
                }
                for( int i = 0; i < m_process_number; ++i )
                {
                    if( sockfd == m_sub_process[i].m_pipefd[0] )
                    {
                        m_sub_process[i].m_busy_ratio = busy_ratio;
                        break;
                    }
                }
                continue;
            }
        }
    }

    for( int i = 0; i < m_process_number; ++i ) //关闭所有fd
    {
        closefd( m_epollfd, m_sub_process[i].m_pipefd[ 0 ] );
    }
    close( m_epollfd );
}

#endif

#ifndef SRVMGR_H
#define SRVMGR_H

#include <map>
#include <arpa/inet.h>
#include "fdwrapper.h"
#include "conn.h"

using std::map;

class host //远程服务器的信息
{
public:
    char m_hostname[1024];
    int m_port;
    int m_conncnt; //远端服务器允许接收的连接数
};

//mgr管理和远程服务器的连接，进程池使用mgr来管理一组客户连接
class mgr
{
public:
    mgr( int epollfd, const host& srv );
    ~mgr();
    int conn2srv( const sockaddr_in& address ); //和远端服务器进行连接
    conn* pick_conn( int sockfd ); //从和远程服务器连接的conn中挑选一个出来
    void free_conn( conn* connection ); //客户端断开连接后回收对应服务器连接
    int get_used_conn_cnt(); //返回本manager负责的客户端连接和服务器连接总数
    void recycle_conns(); //重用回收的服务器连接
    RET_CODE process( int fd, OP_TYPE type );

private:
    static int m_epollfd;
    map< int, conn* > m_conns; //可以使用的服务器连接
    map< int, conn* > m_used; //已经使用的服务器连接和客户端连接
    map< int, conn* > m_freed; //被回收的服务器连接
    host m_logic_srv; //远端服务器（也就是逻辑服务器），本服务器本身是个中转站
};

#endif

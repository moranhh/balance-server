#ifndef CONN_H
#define CONN_H

#include <arpa/inet.h>
#include "fdwrapper.h"

//记录连接信息的类，同时包含了连接的客户端的fd和连接的服务器的fd
class conn
{
public:
    conn();
    ~conn();
    void init_clt( int sockfd, const sockaddr_in& client_addr ); //初始化客户端连接
    void init_srv( int sockfd, const sockaddr_in& server_addr ); //初始化服务器连接
    void reset(); //将给连接类置空
    //读写客户端
    RET_CODE read_clt();
    RET_CODE write_clt();
    //读写服务器
    RET_CODE read_srv();
    RET_CODE write_srv();

public:
    static const int BUF_SIZE = 2048;

    //客户端缓冲区接收从客户端读来的数据，read_idx表示从客户端读取了多少数据，
    //然后conn将客户端缓冲区的数据发往服务器，write_idx表示发送了多少数据,服务器缓冲区类似
    char* m_clt_buf; //客户端缓冲区
    int m_clt_read_idx;
    int m_clt_write_idx;
    sockaddr_in m_clt_address; //客户端socket地址
    int m_cltfd; //客户端fd

    char* m_srv_buf; //服务器缓冲区
    int m_srv_read_idx;
    int m_srv_write_idx;
    sockaddr_in m_srv_address;
    int m_srvfd;

    bool m_srv_closed; //服务器是否关闭
};

#endif

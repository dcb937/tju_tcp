#include "tju_tcp.h"

sendTimer* timerQueue = NULL;
/*
创建 TCP socket
初始化对应的结构体
设置初始状态为 CLOSED
*/
tju_tcp_t* tju_socket()
{
    tju_tcp_t* sock = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED;

    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = NULL;
    sock->sending_len = 0;

    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = NULL;
    sock->received_len = 0;

    if (pthread_cond_init(&sock->wait_cond, NULL) != 0) {
        perror("ERROR condition variable not set\n");
        exit(-1);
    }

    sock->window.wnd_send = NULL;
    sock->window.wnd_recv = NULL;

    return sock;
}

/*
绑定监听的地址 包括ip和端口
*/
int tju_bind(tju_tcp_t* sock, tju_sock_addr bind_addr)
{
    sock->bind_addr = bind_addr;
    return 0;
}

/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t* sock)
{
    sock->state = LISTEN;
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    listen_socks[hashval] = sock;
    // 建立半连接和全连接队列
    syn_queues[hashval] = malloc(sizeof(syn_queue_member*) * MAX_SYN_QUEUE_LENGTH);
    accept_queues[hashval] = malloc(sizeof(tju_tcp_t*) * MAX_ACCEPT_QUEUE_LENGTH);
    return 0;
}

/*
接受连接
返回与客户端通信用的socket
这里返回的socket一定是已经完成3次握手建立了连接的socket
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
tju_tcp_t* tju_accept(tju_tcp_t* listen_sock)
{
    // tju_tcp_t* new_conn = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    // memcpy(new_conn, listen_sock, sizeof(tju_tcp_t)); // 所有新建立的socket和这个监听socket共用一个send/recv锁？

    // tju_sock_addr local_addr, remote_addr;
    /*
     这里涉及到TCP连接的建立
     正常来说应该是收到客户端发来的SYN报文
     从中拿到对端的IP和PORT
     换句话说 下面的处理流程其实不应该放在这里 应该在tju_handle_packet中
    */

    if (server_event_log == NULL)
        if ((server_event_log = fopen("server.event.trace", "w")) == NULL) {
            printf("Fail to open file!\n");
            exit(-1);
        }

    while (accept_queues[cal_hash(listen_sock->bind_addr.ip, listen_sock->bind_addr.port, 0, 0)][0] == NULL)
        ; // 当全连接队列里面有值的时候，跳出阻塞
    tju_tcp_t* new_conn = accept_queues[cal_hash(listen_sock->bind_addr.ip, listen_sock->bind_addr.port, 0, 0)][0];

    // 从全连接队列中取出
    accept_queues[cal_hash(listen_sock->bind_addr.ip, listen_sock->bind_addr.port, 0, 0)][0] = NULL;
    // 这里应该是经过三次握手后才能修改状态为ESTABLISHED
    // new_conn->state = ESTABLISHED;

    // 将新的conn放到内核建立连接的socket哈希表中
    // int hashval = cal_hash(local_addr.ip, local_addr.port, remote_addr.ip, remote_addr.port);
    // established_socks[hashval] = new_conn;

    // 如果new_conn的创建过程放到了tju_handle_packet中 那么accept怎么拿到这个new_conn呢
    // 在linux中 每个listen socket都维护一个已经完成连接的socket队列
    // 每次调用accept 实际上就是取出这个队列中的一个元素
    // 队列为空,则阻塞

    // 初始化发送窗口和接受窗口

    new_conn->window.wnd_recv = malloc(sizeof(receiver_window_t));
    new_conn->window.wnd_send = malloc(sizeof(sender_window_t));

    new_conn->window.wnd_send->timeout_interval.tv_sec = 0;
    new_conn->window.wnd_send->timeout_interval.tv_usec = 50000;
    pthread_mutex_init(&(new_conn->window.wnd_send->timer_lock), NULL);
    new_conn->window.wnd_send->rwnd = TCP_RECVWN_SIZE;

    new_conn->window.wnd_send->cwnd = MAX_DLEN;
    new_conn->window.wnd_send->ssthresh = MAX_DLEN * 32;
    new_conn->window.wnd_send->congestion_status = 0;

    new_conn->window.wnd_send->base = 464 + 1;
    new_conn->window.wnd_send->base_ptr = new_conn->window.wnd_send->buf;
    new_conn->window.wnd_send->nextseq = 464 + 1;
    new_conn->window.wnd_send->nextseq_ptr = new_conn->window.wnd_send->buf;

    new_conn->window.wnd_send->ack_sent = tmp_ack; // TODO:
                                                   // 或许用宏表示更好

    new_conn->window.wnd_recv->expect_seq = tmp_ack;

    new_conn->sending_buf = new_conn->window.wnd_send->buf;
    new_conn->received_buf = new_conn->window.wnd_recv->buf; // TODO: 东西初始化的有点多， 或许放在一个函数里面更好

    // pthread_t thread_id = SERVER_SEND_PTHREAD_ID; // 建立后台线程负责处理发送窗口
    int rst = pthread_create(&SERVER_SEND_PTHREAD_ID, NULL, send_thread, (void*)new_conn);
    if (rst < 0) {
        printf("fail create thread\n");
    }
    return new_conn;
}

/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t* sock, tju_sock_addr target_addr)
{
    if (client_event_log == NULL)
        if ((client_event_log = fopen("client.event.trace", "w")) == NULL) {
            printf("Fail to open file!\n");
            exit(-1);
        }

    sock->established_remote_addr = target_addr;

    tju_sock_addr local_addr;
    local_addr.ip = inet_network("172.17.0.2");
    local_addr.port = 5678; // 连接方进行connect连接的时候 内核中是随机分配一个可用的端口
    sock->established_local_addr = local_addr;

    // 将建立了连接的socket放入内核 已建立连接哈希表中
    int hashval = cal_hash(local_addr.ip, local_addr.port, target_addr.ip, target_addr.port);
    established_socks[hashval] = sock;

    sock->state = SYN_SENT;
    int seq = 876; // 应当是一个随机数
    // printf("%d %d\n", sock->established_local_addr.port, sock->established_remote_addr.port);
    printf("client send SYN\n");
    char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                  seq, 0, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, SYN, TCP_RECVWN_SIZE, 0, NULL, 0);
    packet_buf_send_with_timer(msg, DEFAULT_HEADER_LEN, 1.0);
    // 这里也不能直接建立连接 需要经过三次握手
    // 实际在linux中 connect调用后 会进入一个while循环
    // 循环跳出的条件是socket的状态变为ESTABLISHED 表面看上去就是 正在连接中 阻塞
    // 而状态的改变在别的地方进行 在我们这就是tju_handle_packet

    while (sock->state != ESTABLISHED)
        ;

    // 初始化窗口

    sock->window.wnd_recv = malloc(sizeof(receiver_window_t));
    sock->window.wnd_send = malloc(sizeof(sender_window_t));

    sock->window.wnd_send->timeout_interval.tv_sec = 0;
    sock->window.wnd_send->timeout_interval.tv_usec = 50000;
    pthread_mutex_init(&(sock->window.wnd_send->timer_lock), NULL);
    sock->window.wnd_send->rwnd = TCP_RECVWN_SIZE;

    sock->window.wnd_send->cwnd = MAX_DLEN;
    sock->window.wnd_send->ssthresh = MAX_DLEN * 32;
    sock->window.wnd_send->congestion_status = 0;

    sock->window.wnd_send->base = 876 + 2;
    sock->window.wnd_send->base_ptr = sock->window.wnd_send->buf;
    sock->window.wnd_send->nextseq = 876 + 2;
    sock->window.wnd_send->nextseq_ptr = sock->window.wnd_send->buf;

    sock->window.wnd_send->ack_sent = tmp_ack;

    sock->window.wnd_recv->expect_seq = tmp_ack;

    sock->sending_buf = sock->window.wnd_send->buf;
    sock->received_buf = sock->window.wnd_recv->buf;

    // pthread_t thread_id = CLIENT_SEND_PTHREAD_ID; // 建立后台线程负责处理发送窗口

    int rst = pthread_create(&CLIENT_SEND_PTHREAD_ID, NULL, send_thread, (void*)sock);
    if (rst < 0) {
        printf("fail create thread\n");
    }

    return 0;
}

void* send_thread(void* args)
{
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL); // 接受到cancel信号立即停止

    tju_tcp_t* sock = (tju_tcp_t*)args;
    sender_window_t* wnd = sock->window.wnd_send;
    // printf("send_thread\'s base_ptr: %p\n", wnd->base_ptr);

    while (1) {

        // 判断是否超时
        while (pthread_mutex_lock(&(wnd->timer_lock)) != 0)
            ;
        // 确保队列存在
        if (timerQueue != NULL) {
            struct timeval nowTimer, subTimer;
            gettimeofday(&nowTimer, NULL); // 获取当前时间
            timersub(&nowTimer, &(timerQueue->updateTime), &subTimer);
            if (timercmp(&subTimer, &(timerQueue->RTO), >)) {
                // 检测到第一个包超时,回退N帧
                printf("timeout\n");
                wnd->ssthresh = wnd->cwnd / 2;
                wnd->cwnd = MAX_DLEN;
                wnd->ack_cnt = 0;
                wnd->congestion_status = SLOW_START;
                write_log_CWND(CONGESTION_TIMEOUT, wnd->cwnd);

                // 四次握手的Timewait，一旦到达倒计时则close，释放资源
                if (timerQueue->flags == 0b11111111) {
                    DPRINTF("release resources\n");
                    int hashval = cal_hash(sock->established_local_addr.ip, sock->established_local_addr.port,
                                           sock->established_remote_addr.ip, sock->established_remote_addr.port);
                    free(sock->window.wnd_recv);
                    free(sock->window.wnd_send);
                    free(sock);
                    established_socks[hashval] = NULL;
                    pthread_exit(NULL);
                }

                fprintf(client_event_log, "now: %ld, timeout, RTO: %ld\n", getCurrentTime(), timerQueue->RTO.tv_usec);
                sendTimer* reSender = timerQueue;
                while (reSender != NULL) {
                    //  while循环把当前窗口中的数据包全部重发
                    int plen = reSender->len + DEFAULT_HEADER_LEN;
                    // 如果超出范围
                    char* tmp_mem; // 以免出现要发送的数据一段在缓冲区后面，一段在缓冲区前面
                    int remain_len;
                    if (reSender->firstByte + reSender->len < wnd->buf + TCP_SENDWN_SIZE) {
                        tmp_mem = reSender->firstByte;
                    } else {
                        tmp_mem = malloc(reSender->len);
                        remain_len = wnd->buf + TCP_SENDWN_SIZE - reSender->firstByte;
                        memcpy(tmp_mem, reSender->firstByte, remain_len);
                        memcpy(tmp_mem + remain_len, wnd->buf, reSender->len - remain_len);
                    }

                    char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                                  reSender->seq, reSender->ack,
                                                  DEFAULT_HEADER_LEN, plen, reSender->flags, 1, 0, reSender->firstByte, reSender->len);
                    write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), reSender->len);
                    if (tmp_mem != reSender->firstByte)
                        free(tmp_mem);

                    gettimeofday(&(reSender->sendTime), NULL); // 设定发送时间
                    timercpy(&(reSender->updateTime), &(reSender->sendTime));
                    timercpy(&(reSender->RTO), &(sock->window.wnd_send->timeout_interval)); //设定RTO
                    reSender->is_retransmit = 1;
                    printf("retransmit a pkt\n");
                    // fprintf(client_event_log, "retransmit a pkt, pkt's Seq: %d\n", reSender->seq);
                    // print_packet_header(msg);
                    sendToLayer3(msg, plen);
                    reSender = (sendTimer*)reSender->next;
                }
            }
        }
        pthread_mutex_unlock(&(wnd->timer_lock));

        // 判断是否有需要待发送的数据
        while (pthread_mutex_lock(&(sock->send_lock)) != 0)
            ; //加锁

#ifdef CONGESTION_CONTROL
#ifdef FLOW_CONTROL
        if (wnd->nextseq_ptr != sock->sending_buf && wnd->rwnd != 0 && distance_in_sender_window(wnd->base_ptr, wnd->nextseq_ptr) <= wnd->cwnd)
#endif

#ifdef MAX_SEND_PKT_CONTROL
            if (wnd->nextseq_ptr != sock->sending_buf && send_pkt_num < MAX_SEND_PKT && distance_in_sender_window(wnd->base_ptr, wnd->nextseq_ptr) <= wnd->cwnd)
#endif
#endif
#ifndef CONGESTION_CONTROL
#ifdef FLOW_CONTROL
                if (wnd->nextseq_ptr != sock->sending_buf && wnd->rwnd != 0)
#endif

#ifdef MAX_SEND_PKT_CONTROL
                    if (wnd->nextseq_ptr != sock->sending_buf && send_pkt_num < MAX_SEND_PKT)
#endif
#endif

                    {
                        if (wnd->nextseq_ptr < wnd->base_ptr && wnd->nextseq_ptr > sock->sending_buf || wnd->base_ptr < sock->sending_buf && wnd->nextseq_ptr < wnd->base_ptr) {
                            printf("Not expected locations1\n");
                            printf("DEBUG:\n");
                            printf("buf: %p, base_ptr: %p, nextseq_ptr: %p, sock_sending_buf: %p\n", wnd->buf, wnd->base_ptr, wnd->nextseq_ptr, sock->sending_buf);
                            printf("buf_end: %p\n", wnd->buf + TCP_SENDWN_SIZE);
                            exit(-1);
                        }

                        // 一个循环数组的思想，但实现起来有点繁琐。。。。。。
                        // 相当于右边又拼了一个相同大小的缓冲区
                        // 目标: new_nextseq = min{nextseq_ptr + rwnd, nextesq_ptr + MAX_DLEN, sending_buf}
                        // 下面是为了求这三个中的最小值
                        char* tmp_nextseq_ptr = wnd->base_ptr <= wnd->nextseq_ptr ? wnd->nextseq_ptr
                                                                                  : wnd->nextseq_ptr + TCP_SENDWN_SIZE;

                        // printf("wnd->base_ptr: %p, sock->sending_buf: %p\n", wnd->base_ptr, sock->sending_buf);
                        char* tmp_sending_buf = wnd->base_ptr <= sock->sending_buf ? sock->sending_buf
                                                                                   : sock->sending_buf + TCP_SENDWN_SIZE;

                        // printf("\n%p, %p\n", wnd->nextseq_ptr + wnd->rwnd, tmp_nextseq_ptr + MAX_DLEN);
#ifdef FLOW_CONTROL
                        char* min12 = tmp_nextseq_ptr + wnd->rwnd < tmp_nextseq_ptr + MAX_DLEN ? tmp_nextseq_ptr + wnd->rwnd
                                                                                               : tmp_nextseq_ptr + MAX_DLEN;
#endif
#ifdef MAX_SEND_PKT_CONTROL
                        char* min12 = tmp_nextseq_ptr + MAX_DLEN;
#endif
                        // printf("tmp_nextseq_ptr: %p, tmp_sending_buf: %p\n", tmp_nextseq_ptr, tmp_sending_buf);
                        char* min23 = tmp_nextseq_ptr + MAX_DLEN < tmp_sending_buf ? tmp_nextseq_ptr + MAX_DLEN
                                                                                   : tmp_sending_buf;

                        // printf("min12: %p, min23: %p, ", min12, min23);
                        char* min123 = min12 < min23 ? min12 : min23;
                        // printf("min123: %p\n", min123);
                        if (min123 >= wnd->buf + TCP_SENDWN_SIZE)
                            min123 -= TCP_SENDWN_SIZE; // min123 即为 新的 nextseq_ptr

                        int len = wnd->nextseq_ptr <= min123
                                      ? min123 - wnd->nextseq_ptr
                                      : min123 + TCP_SENDWN_SIZE - wnd->nextseq_ptr;

                        if (len <= 0 || len > MAX_DLEN) {
                            printf("Incorrect in length\n");
                            printf("DEBUG:\n");
                            printf("len: %d, buf: %p, base_ptr: %p, old_nextseq_ptr: %p, new_nextseq_ptr: %p, sock_sending_buf: %p\n", len, wnd->buf, wnd->base_ptr, wnd->nextseq_ptr, min123, sock->sending_buf);
                            printf("buf_end: %p\n", wnd->buf + TCP_SENDWN_SIZE);
                            // target: new_nextseq = min{nextseq_ptr + rwnd, nextesq_ptr + MAX_DLEN, sending_buf}
                            printf("min{nextseq_ptr + rwnd, nextesq_ptr + MAX_DLEN}: %p\nmin{nextesq_ptr + MAX_DLEN, sending_buf}: %p\n", min12, min23);
                            printf("min{nextseq_ptr + rwnd, nextesq_ptr + MAX_DLEN, sending_buf}: %p\n", min123);
                            printf("old_rwnd: %d\n", wnd->rwnd);
                            printf("new rwnd: %d\n", wnd->rwnd - len);
                            exit(-1);
                        }

                        uint16_t plen = DEFAULT_HEADER_LEN + len;

                        char* tmp_mem; // 以免出现要发送的数据一段在缓冲区后面，一段在缓冲区前面
                        int remain_len;
                        if (wnd->nextseq_ptr + len < wnd->buf + TCP_SENDWN_SIZE) {
                            tmp_mem = wnd->nextseq_ptr;
                        } else {
                            tmp_mem = malloc(len);
                            remain_len = wnd->buf + TCP_SENDWN_SIZE - wnd->nextseq_ptr;
                            memcpy(tmp_mem, wnd->nextseq_ptr, remain_len);
                            memcpy(tmp_mem + remain_len, wnd->buf, len - remain_len);
                        }

                        char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                                      wnd->nextseq, wnd->ack_sent,
                                                      DEFAULT_HEADER_LEN, plen, ACK, 1, 0, tmp_mem, len);

                        if (tmp_mem != wnd->nextseq_ptr)
                            free(tmp_mem);

                        sendTimer* Timer = (sendTimer*)malloc(sizeof(sendTimer));
                        Timer->len = len;
                        Timer->is_retransmit = 0;
                        Timer->firstByte = wnd->nextseq_ptr;
                        Timer->next = NULL;
                        Timer->ack = wnd->ack_sent;
                        Timer->seq = wnd->nextseq;
                        Timer->flags = ACK;
                        gettimeofday(&(Timer->sendTime), NULL);
                        timercpy(&(Timer->updateTime), &(Timer->sendTime));
                        timercpy(&(Timer->RTO), &(wnd->timeout_interval));

                        while (pthread_mutex_lock(&(wnd->timer_lock)) != 0)
                            ;
                        timerAppend(Timer);
                        pthread_mutex_unlock(&(wnd->timer_lock));

                        wnd->nextseq_ptr = min123; // baseseq则应该在packet_handle中更新
                        wnd->nextseq = wnd->nextseq + len;

                        wnd->rwnd = wnd->rwnd - len;

                        sendToLayer3(msg, plen); // TODO: 滑动窗口的重发，可以考虑令nextseq_ptr=base_ptr,记得同时恢复nextseq
                        write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), len);

                        send_pkt_num++;
                        // printf("thread send a packet, packet\'s seq: %d, ack: %d, len = %d\n", get_seq(msg), get_ack(msg), len);
                        // printf("after send, new nextseq_ptr: %p\n", wnd->nextseq_ptr);
                    }
        pthread_mutex_unlock(&(sock->send_lock));
    }
}

// 将新的计时器加入计时器队列
void timerAppend(sendTimer* T)
{
    if (timerQueue == NULL) {
        timerQueue = T;
        timerQueue->next = NULL;
        return;
    }
    sendTimer* t = timerQueue;
    while (t->next != NULL)
        t = (sendTimer*)t->next;
    t->next = (struct sendTimer*)T;
    T->next = NULL;
}

// 删除首计时器节点
void timerDel()
{
    sendTimer* del = timerQueue;
    timerQueue = (sendTimer*)timerQueue->next;
    free(del);
}

int tju_send(tju_tcp_t* sock, const void* buffer, int len)
{
    // 这里当然不能直接简单地调用sendToLayer3

    while (sock->sending_len + len >= TCP_SENDWN_SIZE)
        ;

    while (pthread_mutex_lock(&(sock->send_lock)) != 0)
        ;

    int remain_len = TCP_SENDWN_SIZE - (sock->sending_buf - sock->window.wnd_send->buf);

    if (sock->sending_buf + len < sock->window.wnd_send->buf + TCP_SENDWN_SIZE) {
        memcpy(sock->sending_buf, buffer, len);
        sock->sending_buf = sock->sending_buf + len;
    } else {
        memcpy(sock->sending_buf, buffer, remain_len);
        memcpy(sock->window.wnd_send->buf, buffer + remain_len, len - remain_len);
        sock->sending_buf = sock->window.wnd_send->buf + len - remain_len;
    }
    sock->sending_len += len;

    write_log_SWND(sock->sending_len);

    pthread_mutex_unlock(&(sock->send_lock));
    return 0;
}

int tju_recv(tju_tcp_t* sock, void* buffer, int len)
{
    while (sock->received_len <= 0) {
        // 阻塞
    }

    while (pthread_mutex_lock(&(sock->recv_lock)) != 0)
        ; // 加锁

    int read_len = 0;
    if (sock->received_len >= len) { // 从中读取len长度的数据
        read_len = len;
    } else {
        read_len = sock->received_len; // 读取sock->received_len长度的数据(全读出来)
    }

    if (sock->received_buf + read_len < sock->window.wnd_recv->buf + TCP_RECVWN_SIZE) { // 确保sock->received_buf指针不越出接受窗口的范围
        memcpy(buffer, sock->received_buf, read_len);
        sock->received_buf += read_len;
        sock->received_len -= read_len;
    } else {
        int remain_len = TCP_RECVWN_SIZE - (sock->received_buf - sock->window.wnd_recv->buf);
        memcpy(buffer, sock->received_buf, remain_len);
        memcpy(buffer + remain_len, sock->window.wnd_recv->buf, read_len - remain_len);
        sock->received_buf = sock->window.wnd_recv->buf + read_len - remain_len;
        sock->received_len -= read_len;
    }

    write_log_RWND(TCP_RECVWN_SIZE - sock->received_len);

    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    return read_len;
}

uint16_t count_rwnd(tju_tcp_t* sock)
{
    return TCP_RECVWN_SIZE - sock->received_len; // 虽然received_len 是int,但大小肯定在uint16的范围内
}

int tju_handle_packet(tju_tcp_t* sock, char* pkt)
{
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0); // server 处于listen状态的socket的哈希
    char hostname[8];
    gethostname(hostname, 8);
    sender_window_t* wnd_send = sock->window.wnd_send;
    // printf("recvive a pkt\n");
    write_log_RECV(get_seq(pkt), get_ack(pkt), get_flags(pkt), get_plen(pkt) - DEFAULT_HEADER_LEN);

    // -----------------------------------------------------------------------------------------------------
    // -----------------------------------------------------------------------------------------------------
    // rdt可靠传输
    // ESTABLISHED状态下的socket不宜分为server和client讨论，因为TCP是双工的
    if (sock->state == ESTABLISHED && get_flags(pkt) == ACK) {

        if (get_plen(pkt) != DEFAULT_HEADER_LEN) { // data_len不为0，接收方收到发送方发来的数据包
            // receive_flag = 1; // 接收方不用维护计时器
            DPRINTF("Server receive msg, msg's Seq: %d\n", get_seq(pkt));

            if (sock->window.wnd_recv->expect_seq == get_seq(pkt)) { // 没有失序
                DPRINTF("Inorder\n");
                fprintf(server_event_log, "Inorder\n");
                uint32_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;
                if (data_len + sock->received_len >= TCP_RECVWN_SIZE) {
                    return 0;
                }

                sock->window.wnd_recv->expect_seq += get_plen(pkt) - DEFAULT_HEADER_LEN;

                int Seq = sock->window.wnd_send->base;
                int Ack = get_seq(pkt) + get_plen(pkt) - DEFAULT_HEADER_LEN;
                uint16_t adv_window = count_rwnd(sock) - (get_plen(pkt) - DEFAULT_HEADER_LEN); // TODO: 后面减去的dlen非常关键，不能丢

                // printf("adv_window: %d\n", adv_window);

                if (adv_window <= MAX_DLEN)
                    adv_window = MAX_DLEN;

                sock->window.wnd_send->ack_sent = Ack;

                char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                              Seq, Ack, DEFAULT_HEADER_LEN,
                                              DEFAULT_HEADER_LEN, ACK, adv_window, 0, NULL, 0);
                sendToLayer3(msg, DEFAULT_HEADER_LEN); // 接收方不用维护计时器，直接调用sendTolayer
                write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);

                // 把收到的数据放到接受缓冲区
                while (sock->received_len + data_len >= TCP_RECVWN_SIZE) // TODO: 应该是没有等于
                    ;

                if (data_len > 0) {
                    while (pthread_mutex_lock(&(sock->recv_lock)) != 0)
                        ; // 加锁
                    write_log_DELV(get_seq(pkt), data_len);
                    // 这里所谓remain_len指的是received_buf + sock->recvived_len到结尾的长度，而不是RWND

                    int remain_len = TCP_RECVWN_SIZE - (sock->received_buf + sock->received_len - sock->window.wnd_recv->buf);

                    char* cpy_dst = sock->received_buf + sock->received_len < sock->window.wnd_recv->buf + TCP_RECVWN_SIZE
                                        ? sock->received_buf + sock->received_len
                                        : sock->received_buf + sock->received_len - TCP_RECVWN_SIZE;
                    // fprintf(server_event_log, "buf\'s begin: %p, buf\'s end: %p, cpy_dst: %p\n", sock->window.wnd_recv->buf, sock->window.wnd_recv->buf + TCP_RECVWN_SIZE, cpy_dst);
                    if (remain_len > data_len) {
                        memcpy(cpy_dst, pkt + DEFAULT_HEADER_LEN, data_len);
                        sock->received_len += data_len;
                    }
                    // memcpy(sock->received_buf + sock->received_len, pkt + DEFAULT_HEADER_LEN, data_len);
                    else {
                        memcpy(cpy_dst, pkt + DEFAULT_HEADER_LEN, remain_len);
                        memcpy(sock->window.wnd_recv->buf, pkt + DEFAULT_HEADER_LEN + remain_len, data_len - remain_len);
                        sock->received_len += data_len;
                    }

                    write_log_RWND(TCP_RECVWN_SIZE - sock->received_len);

                    // if (sock->state == ESTABLISHED && sock->window.wnd_recv != NULL) {
                    //     fprintf(server_event_log, "function end, expect_seq = %d\n\n", sock->window.wnd_recv->expect_seq);
                    // }

                    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁
                }

            } else { // 失序
                DPRINTF("Disorder, expect seq = %d\n", sock->window.wnd_recv->expect_seq);
                fprintf(server_event_log, "Disorder, expect seq = %d\n", sock->window.wnd_recv->expect_seq);

                int Seq = sock->window.wnd_send->base;
                int Ack = sock->window.wnd_recv->expect_seq;
                uint16_t adv_window = count_rwnd(sock);
                // 失序时重发一个包，Ack为期待一个合序的数据序列号
                char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                              Seq, Ack, DEFAULT_HEADER_LEN,
                                              DEFAULT_HEADER_LEN, ACK, adv_window, 0, NULL, 0);
                sendToLayer3(msg, DEFAULT_HEADER_LEN); // 接收方不用维护计时器，直接调用sendTolayer
                write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);

                // 失序则抛弃这个包
                return 0;
            }
        } else { // data_len为0， 是发送方收到ACK包后的反应

            // wnd_send->rwnd = get_advertised_window(pkt) - (wnd_send->nextseq - get_ack(pkt)); // TODO

            if (get_ack(pkt) > wnd_send->base) { // 发送方收到符合期望的ack
                if (timerQueue == NULL) {
                    // 头指针不存在的异常
                    printf("error: timerQueue: NULL\n");
                    exit(-1);
                }
#ifdef CONGESTION_CONTROL
                switch (wnd_send->congestion_status) {
                case SLOW_START:
                    wnd_send->cwnd += MAX_DLEN;
                    write_log_CWND(SLOW_START, wnd_send->cwnd);
                    break;
                case CONGESTION_AVOIDANCE:
                    wnd_send->cwnd += (MAX_DLEN * MAX_DLEN) / wnd_send->cwnd;
                    write_log_CWND(CONGESTION_AVOIDANCE, wnd_send->cwnd);
                    break;
                case FAST_RECOVERY:
                    wnd_send->cwnd = wnd_send->ssthresh;
                    wnd_send->congestion_status = CONGESTION_AVOIDANCE;
                    write_log_CWND(CONGESTION_AVOIDANCE, wnd_send->cwnd);
                    break;
                default:
                    exit(-1);
                    break;
                }
                if (wnd_send->cwnd >= wnd_send->ssthresh)
                    wnd_send->congestion_status = CONGESTION_AVOIDANCE;

#endif
                write_log_RWND(get_advertised_window(pkt));

                wnd_send->ack_cnt = 0;

                while (pthread_mutex_lock(&(wnd_send->timer_lock)) != 0)
                    ;

                while (get_ack(pkt) > timerQueue->seq + timerQueue->len) {
                    timerDel();
                    send_pkt_num--;
                }
                // 关键，计时器的更新操作需要放在timerDel之前，否则可能因为删除后队列为空导致段错误
                //  // TODO: 不确定dev_rtt的更新用的是新的estimated_rtt还是旧的
                if (timerQueue->is_retransmit == 0) { // 仅接收到没有重传过的timer时才会更新timeout_interval
                    struct timeval sample_RTT, nowTime;
                    gettimeofday(&nowTime, NULL);
                    timersub(&nowTime, &(timerQueue->sendTime), &sample_RTT);
                    // fprintf(client_event_log, "sample RTT: %ld\n", sample_RTT.tv_usec);
                    wnd_send->estmated_rtt.tv_usec = 0.875 * wnd_send->estmated_rtt.tv_usec + 0.125 * sample_RTT.tv_usec;
                    wnd_send->dev_rtt.tv_usec = 0.75 * wnd_send->dev_rtt.tv_usec + 0.25 * abs(sample_RTT.tv_usec - wnd_send->estmated_rtt.tv_usec);
                    wnd_send->timeout_interval.tv_usec = wnd_send->estmated_rtt.tv_usec + 4 * wnd_send->dev_rtt.tv_usec;
                    write_log_RTTS((float)sample_RTT.tv_usec / 1000, (float)wnd_send->estmated_rtt.tv_usec / 1000, (float)wnd_send->dev_rtt.tv_usec / 1000, (float)wnd_send->timeout_interval.tv_usec / 1000);
                }
                timerDel();
                send_pkt_num--;

                sendTimer* tmp = timerQueue;
                while (tmp != NULL) {
                    gettimeofday(&(tmp->updateTime), NULL);
                    tmp = (sendTimer*)tmp->next;
                }

                pthread_mutex_unlock(&(wnd_send->timer_lock));

                while (pthread_mutex_lock(&(sock->send_lock)) != 0)
                    ; //加锁
#ifdef FLOW_CONTROL
                // printf("server's rwnd: %d\n", get_advertised_window(pkt));
                // printf("wnd_send->nextseq - get_ack(pkt)=%d\n", wnd_send->nextseq - get_ack(pkt));

                if (get_advertised_window(pkt) >= wnd_send->nextseq - get_ack(pkt))
                    wnd_send->rwnd = get_advertised_window(pkt) - (wnd_send->nextseq - get_ack(pkt));
                else {
                    printf("get_advertised_window(pkt) < wnd_send->nextseq - get_ack(pkt)\n");
                    exit(-1);
                }

                // printf("after, rwnd: %d\n", wnd_send->rwnd);
#endif
                sock->sending_len -= (get_ack(pkt) - wnd_send->base);
                // 移动base
                // 以防指针越出缓冲区范围，循环到数组前
                wnd_send->base_ptr = add_in_sender_window(sock, wnd_send->base_ptr, get_ack(pkt) - wnd_send->base);

                wnd_send->base = get_ack(pkt);
                write_log_SWND(sock->sending_len);
                pthread_mutex_unlock(&(sock->send_lock));

                DPRINTF("receive a expect ACK\n");
            }
            // 本来预备写快速重传的，后来发现并没有要求，故删去
            //             else if (get_ack(pkt) <= wnd_send->base && get_ack(pkt) != last_fast_retransmit_seq) { // 快速重传
            //                 fprintf(client_event_log, "dup ACK\n");
            //                 // 这里之所以添加了last_fast_retransmit_seq是因为往往一旦出现了冗余ACK，就是一大串
            //                 // TODO: 一个比较理想的处理方式是去把缓冲区里面所有相同冗余ACK给清除掉，但没有一个好的方法

            //                 // 通过ack_cnt来实现快速重传
            //                 if (wnd_send->ack_cnt == 2) {
            //                     last_fast_retransmit_seq = wnd_send->base;
            //                     DPRINTF("Fast Retransmit\n");
            //                     fprintf(client_event_log, "Fast Retransmit\n");
            // #ifdef CONGESTION_CONTROL
            // wnd_send->ssthresh = wnd_send->cwnd / 2;
            // wnd_send->cwnd = wnd_send->ssthresh + 3 * MAX_DLEN;
            // wnd_send->congestion_status = FAST_RECOVERY;
            //                     write_log_CWND(FAST_RECOVERY, wnd_send->cwnd);
            // #endif
            //                     while (pthread_mutex_lock(&(wnd_send->timer_lock)) != 0)
            //                         ;
            //                     // 确保队列存在
            //                     if (timerQueue != NULL) {
            //                         struct timeval nowTimer;
            //                         gettimeofday(&nowTimer, NULL); // 获取当前时间

            //                         sendTimer* reSender = timerQueue;
            //                         while (reSender != NULL) {
            //                             //  while循环把当前窗口中的数据包全部重发
            //                             int plen = reSender->len + DEFAULT_HEADER_LEN;
            //                             // 如果超出范围
            //                             char* tmp_mem; // 以免出现要发送的数据一段在缓冲区后面，一段在缓冲区前面
            //                             int remain_len;
            //                             if (reSender->firstByte + reSender->len < wnd_send->buf + TCP_SENDWN_SIZE) {
            //                                 tmp_mem = reSender->firstByte;
            //                             } else {
            //                                 tmp_mem = malloc(reSender->len);
            //                                 remain_len = wnd_send->buf + TCP_SENDWN_SIZE - reSender->firstByte;
            //                                 memcpy(tmp_mem, reSender->firstByte, remain_len);
            //                                 memcpy(tmp_mem + remain_len, wnd_send->buf, reSender->len - remain_len);
            //                             }

            //                             char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
            //                                                           reSender->seq, reSender->ack,
            //                                                           DEFAULT_HEADER_LEN, plen, reSender->flags, 1, 0, reSender->firstByte, reSender->len);
            //                             write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), reSender->len);
            //                             if (tmp_mem != reSender->firstByte)
            //                                 free(tmp_mem);

            //                             gettimeofday(&(reSender->sendTime), NULL); // 设定发送时间
            //                             timercpy(&(reSender->updateTime), &(reSender->sendTime));
            //                             timercpy(&(reSender->RTO), &(sock->window.wnd_send->timeout_interval)); //设定RTO
            //                             reSender->is_retransmit = 1;
            //                             printf("retransmit a pkt\n");
            //                             // fprintf(client_event_log, "retransmit a pkt, pkt's Seq: %d\n", reSender->seq);
            //                             // print_packet_header(msg);
            //                             sendToLayer3(msg, plen);
            //                             reSender = (sendTimer*)reSender->next;
            //                         }
            //                     }
            //                     pthread_mutex_unlock(&(wnd_send->timer_lock));

            //                     wnd_send->ack_cnt = 0;
            //                 } else {
            //                     wnd_send->ack_cnt++;
            //                 }
            //             }
        }
    }

    // -----------------------------------------------------------------------------------------------------
    // -----------------------------------------------------------------------------------------------------
    // 三次握手的建立
    if (strcmp(hostname, "server") == 0) {
        if (sock->state == LISTEN) {
            if (get_flags(pkt) == SYN) {
                receive_flag = 1;
                printf("server receive SYN\n");

                int is_packet_loss_flag = 0;

                tju_sock_addr local_addr, remote_addr;
                remote_addr.ip = inet_network("172.17.0.2"); //具体的IP地址
                remote_addr.port = get_src(pkt);             //端口

                local_addr.ip = sock->bind_addr.ip;     //具体的IP地址
                local_addr.port = sock->bind_addr.port; //端口
                int new_conn_hashval = cal_hash(local_addr.ip, local_addr.port, remote_addr.ip, remote_addr.port);

                for (int i = 0; i < MAX_SYN_QUEUE_LENGTH; i++) {
                    if (syn_queues[hashval][i] != NULL && syn_queues[hashval][i]->sock == established_socks[new_conn_hashval]) {
                        // 已经在半连接队列里面，说明这是第二次握手包丢失，client重发第一次握手包
                        is_packet_loss_flag = 1;
                        DPRINTF("Second handshake pkt lost\n");
                        break;
                    }
                }

                uint32_t seq = 464; // 事实上应当是一个随机数

                if (is_packet_loss_flag == 0) {
                    tju_tcp_t* new_conn = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
                    memcpy(new_conn, sock, sizeof(tju_tcp_t)); // 所有新建立的socket和这个监听socket共用一个send/recv锁？

                    new_conn->established_local_addr = local_addr;
                    new_conn->established_remote_addr = remote_addr;

                    established_socks[new_conn_hashval] = new_conn;

                    new_conn->state = SYN_RECV;

                    // add remote_addr to SYN_QUEUE
                    for (int i = 0; i < MAX_SYN_QUEUE_LENGTH; i++) {
                        if (syn_queues[hashval][i] == NULL) {
                            syn_queues[hashval][i] = malloc(sizeof(syn_queue_member*));
                            syn_queues[hashval][i]->sock = new_conn;
                            syn_queues[hashval][i]->expect_ack = seq + 1;
                            break;
                        }
                    }
                }
                tmp_ack = get_seq(pkt) + 2;
                char* msg = create_packet_buf(local_addr.port, remote_addr.port, seq, get_seq(pkt) + 1, DEFAULT_HEADER_LEN,
                                              DEFAULT_HEADER_LEN, ACK_SYN, TCP_RECVWN_SIZE, 0, NULL, 0);
                packet_buf_send_with_timer(msg, DEFAULT_HEADER_LEN, 1.0);
                printf("server send ACK/SYN\n");
            }
        } else if (sock->state == SYN_RECV) {
            uint32_t expect_ack;
            int pos;

            for (int i = 0; i < MAX_SYN_QUEUE_LENGTH; i++) {
                if (syn_queues[hashval][i]->sock == sock) {
                    expect_ack = syn_queues[hashval][i]->expect_ack;
                    pos = i;
                    break;
                }
            }

            if (get_flags(pkt) == ACK && get_ack(pkt) == expect_ack) {
                receive_flag = 1;
                printf("server receive ACK\n");
                sock->state = ESTABLISHED;
                syn_queues[hashval][pos] = NULL; // 从半连接中移除
                for (int i = 0; i < MAX_ACCEPT_QUEUE_LENGTH; i++) {
                    if (accept_queues[hashval][i] == 0) {
                        accept_queues[hashval][i] = sock; // 加入全连接
                    }
                }
            }
        }
    } else if (strcmp(hostname, "client") == 0) {
        if (sock->state == SYN_SENT) {
            if (get_flags(pkt) == ACK_SYN && get_ack(pkt) == 876 + 1) {
                receive_flag = 1;
                printf("client receive ACK/SYN\n");
                sock->state = ESTABLISHED;
                tmp_ack = get_seq(pkt) + 2;
                printf("client send ACK\n");
                char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                              876 + 1, get_seq(pkt) + 1, DEFAULT_HEADER_LEN,
                                              DEFAULT_HEADER_LEN, ACK, TCP_RECVWN_SIZE, 0, NULL, 0);
                sendToLayer3(msg, DEFAULT_HEADER_LEN);
                write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);

                // TCP三次握手第三次server端不会回应，如果第三次丢失服务器会重新发送第二次握手
                // 故直接使用sendToLayer3，而不是使用带计时器的发送方式
            }
        } else if (sock->state == ESTABLISHED) {
            if (get_flags(pkt) == ACK_SYN && get_ack(pkt) == 876 + 1) { // 如果第三次握手丢失，server会重发ACK_SYN，
                                                                        // 但之前客户端发送第三次握手后状态即从SYN_SENT转变为ESTABLISHED，
                                                                        // 故ESTABLISH状态的client收到重发的ACK_SYN后需要重发第三次握手包
                printf("client receive ACK/SYN\n");
                printf("client send ACK\n");
                tmp_ack = get_seq(pkt) + 2;
                char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                              876 + 1, get_seq(pkt) + 1, DEFAULT_HEADER_LEN,
                                              DEFAULT_HEADER_LEN, ACK, TCP_RECVWN_SIZE, 0, NULL, 0);
                sendToLayer3(msg, DEFAULT_HEADER_LEN);
                write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);
            }
        }
    }

    // -----------------------------------------------------------------------------------------------------
    // -----------------------------------------------------------------------------------------------------
    // TCP四次挥手
    // 被动关闭方

    if (sock->state == ESTABLISHED && (get_flags(pkt) == FIN) || get_flags(pkt) == FIN_ACK) {
        int Seq = get_ack(pkt);
        int Ack = get_seq(pkt) + 1;

        char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                      Seq, Ack, DEFAULT_HEADER_LEN,
                                      DEFAULT_HEADER_LEN, ACK, 1, 0, NULL, 0);
        DPRINTF("receive FIN_ACK | send ACK, change to CLOSE_WAIT\n");
        sendToLayer3(msg, DEFAULT_HEADER_LEN);
        write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);

        sock->state = CLOSE_WAIT;
        // TCP处于半关闭状态

        // 如果被动close的一方已经没有数据要发送了，则发送FIN_ACK
        if (sock->sending_len == 0) {
            msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                    Seq, Ack, DEFAULT_HEADER_LEN,
                                    DEFAULT_HEADER_LEN, FIN_ACK, 1, 0, NULL, 0);
            sendToLayer3(msg, DEFAULT_HEADER_LEN);
            write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);
            sock->state = LAST_ACK;
            DPRINTF("send ACK, change to LAST_ACK\n");

            sendTimer* Timer = (sendTimer*)malloc(sizeof(sendTimer));
            Timer->len = 0;
            Timer->is_retransmit = 0;
            Timer->firstByte = NULL;
            Timer->next = NULL;
            Timer->ack = Ack;
            Timer->seq = Seq;
            Timer->flags = FIN_ACK;
            gettimeofday(&(Timer->sendTime), NULL);
            timercpy(&(Timer->RTO), &(wnd_send->timeout_interval));
            while (pthread_mutex_lock(&(wnd_send->timer_lock)) != 0)
                ;
            timerAppend(Timer);
            pthread_mutex_unlock(&(wnd_send->timer_lock));
        } else {
            // 如果close的一方还有数据需要发送，则等待数据发送完后再发送FIN_ACK
            // 由于测试程序中不涉及到这个可能，不再考虑
        }
        return 0;
    } else if (sock->state == LAST_ACK && get_flags(pkt) == ACK) {
        timerDel();
        DPRINTF("receive ACK | close, release resources\n");
        int res;
        int hashval = cal_hash(sock->established_local_addr.ip, sock->established_local_addr.port,
                               sock->established_remote_addr.ip, sock->established_remote_addr.port);
        if (strcmp(hostname, "client") == 0) {
            res = pthread_cancel(CLIENT_SEND_PTHREAD_ID);
            if (res != 0) {
                printf("Failed to send cancel signal\n");
                exit(-1);
            }
            res = pthread_join(CLIENT_SEND_PTHREAD_ID, NULL); // 阻塞，等待线程确实结束后才能进行后面的free
        } else if (strcmp(hostname, "server") == 0) {
            res = pthread_cancel(SERVER_SEND_PTHREAD_ID);
            if (res != 0) {
                printf("Failed to send cancel signal\n");
                exit(-1);
            }
            res = pthread_join(SERVER_SEND_PTHREAD_ID, NULL); // 阻塞，等待线程确实结束后才能进行后面的free
        }
        free(sock->window.wnd_recv);
        free(sock->window.wnd_send);
        free(sock);
        established_socks[hashval] = NULL;
        return 0;
    }

    // 主动关闭方
    if (sock->state == FIN_WAIT_1 && get_flags(pkt) == ACK && get_seq(pkt) == wnd_send->ack_sent - 1) {
        timerDel();
        DPRINTF("receive ACK | change to FIN_WAIT_2\n");
        sock->state = FIN_WAIT_2;
        return 0;
    } // 由于FIN_WAIT_1到FIN_WAIT_2没有设置timer重发，可能丢失，故FIN_WAIT1可能会直接接受到FIN_ACK
    // 这里的get_ack(pkt) == wnd_send->base + 1是为了与同时关闭里面的FIN_ACK相区别开
    else if ((sock->state == FIN_WAIT_1 || sock->state == FIN_WAIT_2) && get_flags(pkt) == FIN_ACK && get_ack(pkt) == wnd_send->base) {

        sock->state = TIME_WAIT;

        int Seq = get_ack(pkt);
        int Ack = get_seq(pkt) + 1;

        char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                      Seq, Ack, DEFAULT_HEADER_LEN,
                                      DEFAULT_HEADER_LEN, ACK, 1, 0, NULL, 0);
        sendToLayer3(msg, DEFAULT_HEADER_LEN);
        DPRINTF("receive FIN_ACK | send ACK, change to TIME_WAIT\n");
        write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);

        // 设置定时器，一段时间后进入CLOSE
        sendTimer* Timer = (sendTimer*)malloc(sizeof(sendTimer));
        Timer->len = 0;
        Timer->is_retransmit = 0;
        Timer->firstByte = NULL;
        Timer->next = NULL;
        Timer->ack = Ack;
        Timer->seq = Seq;
        Timer->flags = 0b11111111; // 随便设置的的一个flag
        gettimeofday(&(Timer->sendTime), NULL);

        Timer->RTO.tv_usec = 2000000; // 2s
        while (pthread_mutex_lock(&(wnd_send->timer_lock)) != 0)
            ;
        timerAppend(Timer);
        pthread_mutex_unlock(&(wnd_send->timer_lock));

        return 0;
    } // FIN_WAIT_2发送的包可能丢失，导致被关闭方在TIME_WAIT阶段可能收到重发的FIN_ACK
    else if (sock->state == TIME_WAIT && get_flags(pkt) == FIN_ACK && get_seq(pkt) == wnd_send->ack_sent) {
        int Seq = get_ack(pkt);
        int Ack = get_seq(pkt) + 1;

        char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                      Seq, Ack, DEFAULT_HEADER_LEN,
                                      DEFAULT_HEADER_LEN, ACK, 1, 0, NULL, 0);
        sendToLayer3(msg, DEFAULT_HEADER_LEN);
        DPRINTF("receive FIN_ACK | send ACK\n");
        write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);

        return 0;
    }

    // 同时关闭
    if (sock->state == FIN_WAIT_1 && (get_flags(pkt) == FIN_ACK || get_flags(pkt) == FIN_FLAG_MASK) && get_seq(pkt) == wnd_send->ack_sent - 1) {
        timerDel();
        sock->state = CLOSING;
        int Seq = get_ack(pkt) + 1;
        int Ack = get_seq(pkt) + 1;

        char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                      Seq, Ack, DEFAULT_HEADER_LEN,
                                      DEFAULT_HEADER_LEN, ACK, 1, 0, NULL, 0);
        sendToLayer3(msg, DEFAULT_HEADER_LEN);
        DPRINTF("receive FIN_ACK | send ACK, change to CLOSING\n");
        sendTimer* Timer = (sendTimer*)malloc(sizeof(sendTimer));
        Timer->len = 0;
        Timer->is_retransmit = 0;
        Timer->firstByte = NULL;
        Timer->next = NULL;
        Timer->ack = Ack;
        Timer->seq = Seq;
        Timer->flags = ACK; // 随便设置的的一个flag
        gettimeofday(&(Timer->sendTime), NULL);
        timercpy(&(Timer->RTO), &(wnd_send->timeout_interval));
        while (pthread_mutex_lock(&(wnd_send->timer_lock)) != 0)
            ;
        timerAppend(Timer);
        pthread_mutex_unlock(&(wnd_send->timer_lock));
        return 0;
    } else if (sock->state == CLOSING && get_flags(pkt) == ACK) {
        timerDel();
        sock->state = TIME_WAIT;
        int Seq = get_ack(pkt);
        int Ack = get_seq(pkt) + 1;

        // char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
        //                               Seq, Ack, DEFAULT_HEADER_LEN,
        //                               DEFAULT_HEADER_LEN, ACK, 1, 0, NULL, 0);
        // sendToLayer3(msg, DEFAULT_HEADER_LEN);
        DPRINTF("receive ACK | change to TIME_WAIT\n");
        // write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);
        // 设置定时器，一段时间后进入CLOSE
        sendTimer* Timer = (sendTimer*)malloc(sizeof(sendTimer));
        Timer->len = 0;
        Timer->is_retransmit = 0;
        Timer->firstByte = NULL;
        Timer->next = NULL;
        Timer->ack = Ack;
        Timer->seq = Seq;
        Timer->flags = 0b11111111; // 随便设置的的一个flag
        gettimeofday(&(Timer->sendTime), NULL);

        Timer->RTO.tv_usec = 2000000; // 2s
        while (pthread_mutex_lock(&(wnd_send->timer_lock)) != 0)
            ;
        timerAppend(Timer);
        pthread_mutex_unlock(&(wnd_send->timer_lock));
        return 0;
    } // 如果对面丢包
    // else if (sock->state == TIME_WAIT && get_flags(pkt) == ACK && get_seq(pkt) == wnd_send->ack_sent) {
    //     int Seq = get_ack(pkt);
    //     int Ack = get_seq(pkt) + 1;

    //     char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
    //                                   Seq, Ack, DEFAULT_HEADER_LEN,
    //                                   DEFAULT_HEADER_LEN, ACK, 1, 0, NULL, 0);
    //     DPRINTF("receive ACK | send ACK\n");
    //     sendToLayer3(msg, DEFAULT_HEADER_LEN);
    //     write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);
    // }

    return 0;
}

int tju_close(tju_tcp_t* sock)
{
    sender_window_t* wnd = sock->window.wnd_send;
    fprintf(client_event_log, "wnd_send->base-1: %d, wnd->ack_sent-1: %d\n", wnd->base - 1, wnd->ack_sent - 1);
    // 这里发送的ack是ack_sent-1，base应该是base-1,之前在rdt设计初始值的时候应该都大了1，也不改了，以免出bug
    char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                  wnd->base - 1, wnd->ack_sent - 1, DEFAULT_HEADER_LEN,
                                  DEFAULT_HEADER_LEN, FIN_ACK, 1, 0, NULL, 0);
    sendToLayer3(msg, DEFAULT_HEADER_LEN);
    DPRINTF("send FIN_ACK, change to FIN_WAIT_1\n");
    write_log_SEND(get_seq(msg), get_ack(msg), get_flags(msg), 0);
    sendTimer* Timer = (sendTimer*)malloc(sizeof(sendTimer));
    Timer->len = 0;
    Timer->is_retransmit = 0;
    Timer->firstByte = NULL;
    Timer->next = NULL;
    Timer->ack = wnd->ack_sent;
    Timer->seq = wnd->base;
    Timer->flags = FIN_ACK;
    gettimeofday(&(Timer->sendTime), NULL);
    timercpy(&(Timer->RTO), &(wnd->timeout_interval));
    while (pthread_mutex_lock(&(wnd->timer_lock)) != 0)
        ;
    timerAppend(Timer);
    pthread_mutex_unlock(&(wnd->timer_lock));

    sock->state = FIN_WAIT_1;

    return 0;
}

double timeval_diff(struct timeval* tv0, struct timeval* tv1)
{
    double time1, time2;

    time1 = tv0->tv_sec + (tv0->tv_usec / 1000000.0);
    time2 = tv1->tv_sec + (tv1->tv_usec / 1000000.0);

    time1 = time1 - time2;
    if (time1 < 0)
        time1 = -time1;
    return time1;
}

// 最初对计时器的设想，后来采用了计时器队列用于rdt，这部分计时器的设计便只用于实验的第一阶段了
// 该计时器递归调用重传
void* set_timer_with_retransmit_thread(void* args)
{
    timer_args* tmp = (timer_args*)args;
    struct timeval set_time, now_time;
    gettimeofday(&set_time, NULL);
    while (1) {
        gettimeofday(&now_time, NULL);
        if (timeval_diff(&now_time, &set_time) >= tmp->timeout) {
            printf("timeout\n");
            packet_buf_send_with_timer(tmp->buf, tmp->len, tmp->timeout);
            free(tmp);
            break;
        }
        if (receive_flag) {
            free(tmp);
            free(tmp->buf); // create_packet_buf所返回的buf是由malloc分配的，需要free
            break;
        }
    }
}

// 面对丢包时有处理，但不适应于滑动窗口的数据传输，滑动窗口的数据传输依靠sender_window的变量来协调
void packet_buf_send_with_timer(char* packet_buf, int packet_len, double timeoutInterval)
{
    pthread_t thread_id;
    receive_flag = 0;
    timer_args* args = malloc(sizeof(timer_args)); // 保存packet，以便超时后的重传
    args->buf = packet_buf;
    args->len = packet_len;
    args->timeout = timeoutInterval;
    sendToLayer3(packet_buf, packet_len);
    write_log_SEND(get_seq(args->buf), get_ack(args->buf), get_flags(args->buf), 0);

    int rst = pthread_create(&thread_id, NULL, set_timer_with_retransmit_thread, (void*)args);
    if (rst < 0) {
        printf("ERROR open thread");
        exit(-1);
    }
}

long getCurrentTime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

void write_log_SEND(uint32_t seq, uint32_t ack, uint8_t flag, int size)
{
    char hostname[8];
    gethostname(hostname, 8);
    long current_time = getCurrentTime();
    char flag_str[10];
    if (flag == ACK)
        strcpy(flag_str, "ACK");
    else if (flag == SYN)
        strcpy(flag_str, "SYN");
    else if (flag == ACK_SYN)
        strcpy(flag_str, "ACK|SYN");
    else if (flag == FIN_ACK)
        strcpy(flag_str, "FIN|ACK");

    if (strcmp(hostname, "server") == 0) {
        fprintf(server_event_log, "[%ld] [SEND] [seq:%d ack:%d flag:%d length:%d]\n", current_time, seq, ack, flag, size);
        // fflush(server_event_log);
    } else if (strcmp(hostname, "client") == 0) {
        fprintf(client_event_log, "[%ld] [SEND] [seq:%d ack:%d flag:%d length:%d]\n", current_time, seq, ack, flag, size);
        // fflush(client_event_log);
    }
}

void write_log_RECV(uint32_t seq, uint32_t ack, uint8_t flag, int size)
{
    char hostname[8];
    gethostname(hostname, 8);
    long current_time = getCurrentTime();
    char flag_str[10];
    if (flag == ACK)
        strcpy(flag_str, "ACK");
    else if (flag == SYN)
        strcpy(flag_str, "SYN");
    else if (flag == ACK_SYN)
        strcpy(flag_str, "ACK|SYN");
    else if (flag == FIN_ACK)
        strcpy(flag_str, "FIN|ACK");

    if (strcmp(hostname, "server") == 0) {
        fprintf(server_event_log, "[%ld] [RECV] [seq:%d ack:%d flag:%d length:%d]\n", current_time, seq, ack, flag, size);
        // fflush(server_event_log);
    } else if (strcmp(hostname, "client") == 0) {
        fprintf(client_event_log, "[%ld] [RECV] [seq:%d ack:%d flag:%d length:%d]\n", current_time, seq, ack, flag, size);
        // fflush(client_event_log);
    }
}

void write_log_CWND(int type, int size)
{
    char hostname[8];
    gethostname(hostname, 8);
    long current_time = getCurrentTime();

    if (strcmp(hostname, "server") == 0) {
        fprintf(server_event_log, "[%ld] [CWND] [type:%d size:%d]\n", current_time, type, size);
        //  fflush(server_event_log);
    } else if (strcmp(hostname, "client") == 0) {
        fprintf(client_event_log, "[%ld] [CWND] [type:%d size:%d]\n", current_time, type, size);
        // fflush(client_event_log);
    }
}

void write_log_RWND(int size)
{
    char hostname[8];
    gethostname(hostname, 8);
    long current_time = getCurrentTime();

    if (strcmp(hostname, "server") == 0) {
        fprintf(server_event_log, "[%ld] [RWND] [size:%d]\n", current_time, size);
        //  fflush(server_event_log);
    } else if (strcmp(hostname, "client") == 0) {
        fprintf(client_event_log, "[%ld] [RWND] [size:%d]\n", current_time, size);
        // fflush(client_event_log);
    }
}

void write_log_DELV(uint32_t seq, int size)
{
    char hostname[8];
    gethostname(hostname, 8);
    long current_time = getCurrentTime();

    if (strcmp(hostname, "server") == 0) {
        fprintf(server_event_log, "[%ld] [DELV] [seq:%d size:%d]\n", current_time, seq, size);
        // fflush(server_event_log);
    } else if (strcmp(hostname, "client") == 0) {
        fprintf(client_event_log, "[%ld] [DELV] [seq:%d size:%d]\n", current_time, seq, size);
        // fflush(client_event_log);
    }
}

void write_log_SWND(int size)
{
    char hostname[8];
    gethostname(hostname, 8);
    long current_time = getCurrentTime();

    if (strcmp(hostname, "server") == 0) {
        fprintf(server_event_log, "[%ld] [SWND] [size:%d]\n", current_time, size);
        // fflush(server_event_log);
    } else if (strcmp(hostname, "client") == 0) {
        fprintf(client_event_log, "[%ld] [SWND] [size:%d]\n", current_time, size);
        // fflush(client_event_log);
    }
}

void write_log_RTTS(float SampleRTT, float EstimatedRTT, float DeviationRTT, float TimeoutInterval)
{
    char hostname[8];
    gethostname(hostname, 8);
    long current_time = getCurrentTime();

    if (strcmp(hostname, "server") == 0) {
        fprintf(server_event_log, "[%ld] [RTTS] [SampleRTT:%f EstimatedRTT:%f DeviationRTT:%f TimeoutInterval:%f]\n", current_time, SampleRTT, EstimatedRTT, DeviationRTT, TimeoutInterval);
        // fflush(server_event_log);
    } else if (strcmp(hostname, "client") == 0) {
        fprintf(client_event_log, "[%ld] [RTTS] [SampleRTT:%f EstimatedRTT:%f DeviationRTT:%f TimeoutInterval:%f]\n", current_time, SampleRTT, EstimatedRTT, DeviationRTT, TimeoutInterval);
        // fflush(client_event_log);
    }
}

// 因为发送窗口和接受窗口都是一个循环数组的思想，指针在尽头需要回到缓冲区开头
// 循环数组中两个指针之间的距离，其中lower是要实际小于higher的
int distance_in_sender_window(char* lower, char* higher)
{
    if (lower <= higher) {
        return higher - lower;
    } else {
        return TCP_SENDWN_SIZE - (lower - higher);
    }
}

// 因为发送窗口和接受窗口都是一个循环数组的思想，指针在尽头需要回到缓冲区开头
// 返回的是pos指针增加了num后的结果指针的位置
char* add_in_sender_window(tju_tcp_t* sock, char* pos, int num)
{
    if (pos + num >= sock->window.wnd_send->buf + TCP_SENDWN_SIZE) {
        return pos + num - TCP_SENDWN_SIZE;
    } else {
        return pos + num;
    }
}

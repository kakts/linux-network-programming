/**
 * 9.10.1 UDP/IP マルチキャスト用サーバ
 * 
 * クライアントがマルチキャストに送信し、
 * これはあくまで受信側
 */
/**
 * ヘッダファイルのインクルード
 * はじめに必要なヘッダファイルをインクルードする
 */
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/**
 * UPDマルチキャストサーバソケットの準備
 * 
 * 指定したポートでbind()し、マルチキャストグループへの参加はsetsockopt()で
 * IP_ADD_MEMBERSHIPを行う。
 */
int udp_server_socket_mcast(const char *m_address,
                            const char *portnm,
                            const char *if_address)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    struct ip_mreq mreq;
    int soc, opt, errcode;
    socklen_t opt_len;

    // アドレス情報のヒントをゼロクリア
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    // アドレス情報の決定
    if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (-1);
    }
    if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen,
                        nbuf, sizeof(nbuf),
                        sbuf, sizeof(sbuf),
                        NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        (void) fprintf(stderr, "getnameinfo():%s\n", gai_strerror(errcode));
        freeaddrinfo(res0);
        return (-1);
    }
    (void) fprintf(stderr, "port=%s\n", sbuf);
    // ソケットの生成
    if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) == -1) {
        perror("socket");
        freeaddrinfo(res0);
        return (-1);
    }
    // ソケットオプション(再利用フラグ)設定
    opt = 1;
    opt_len = sizeof(opt);
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }
    // ソケットにアドレスを指定
    if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }

    // マルチキャストグループに参加
    inet_pton(AF_INET, m_address, &mreq.imr_multiaddr);
    inet_pton(AF_INET, if_address, &mreq.imr_interface);
    if (setsockopt(soc, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == -1) {
        perror("setsockopt");
        (void) close(soc);
        freeaddrinfo(res0);
        return (-1);
    }
    freeaddrinfo(res0);
    return (soc);
}

/**
 * 送受信
 * これまでのサンプルと同様
 * 
 */

/**
 * 送受信
 * 
 * TCP/IPと同様に、受信したデータを表示し、:OKをつけて送り返す動きにした。
 * 
 * UDP/IPの場合はコネクションレスプロトコルなので、ソケットが特定の通信相手と結びついていない
 * そのため、送受信の度に宛先や送信元の情報が必要。
 * 
 * 引数に宛先や送信元の情報が指定できるsendto(),recvfrom()を使用
 * 
 * UDP/IPではデータが必ず相手に届く保証がないため、sendto()はソケットディスクリプタが
 * ブロッキング・ノンブロッキングモードでもブロックされることはない
 * sendto()は送信したサイズを返す。
 * recvfrom()はブロッキングモードでは1パケット受信できた場合に戻る。
 * ノンブロッキングの場合は1パケットも受信できない状態でもすぐに戻る。
 */

// サイズ指定文字列連結
size_t mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;

    for (pd = dst, lest = size; *pd != '\0' && lest != 0; pd++, lest--);
    dlen = pd - dst;
    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }
    pde = dst + size - 1;
    for (ps = src; *ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }
    for (; pd <= pde; pd++) {
        *pd = '\0';
    }
    while (*ps++);
    return (dlen + (ps - src - 1));
}

// 送受信
void send_recv_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    char buf[512], *ptr;
    struct sockaddr_storage from;
    ssize_t len;
    socklen_t fromlen;
    // 送受信
    for (;;) {
        // 受信
        fromlen = sizeof(from);
        if ((len = recvfrom(soc,
                            buf,
                            sizeof(buf),
                            0,
                            (struct sockaddr *) &from,
                            &fromlen
                            )) == -1) {
            // error
            perror("recvfrom");
        }
        (void) getnameinfo((struct sockaddr *) &from, fromlen,
                            hbuf, sizeof(hbuf),
                            sbuf, sizeof(sbuf),
                            NI_NUMERICHOST | NI_NUMERICSERV);
        (void) fprintf(stderr, "recvfrom:%s:%s:len=%d\n", hbuf, sbuf, (int) len);

        // 文字列化・表示
        buf[len] = '\0';
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }
        (void) fprintf(stderr, "[client]%s\n", buf);
        // 応答文字列作成
        (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
        len = strlen(buf);
        // 応答
        if ((len = sendto(soc,
                          buf,
                          len,
                          0,
                          (struct sockaddr *) &from,
                          fromlen)) == -1) {
            // error
            perror("sendto");
            break;
        }
    }
}

/**
 * main
 * 起動時引数にマルチキャストアドレス、ポート、インタフェースアドレスが指定されているのを確認
 * udp_server_socket_mcast()でマルチキャストサーバソケットの準備を行う。
 * 
 * 送受信後、マルチキャストグループから脱退し、ソケットクローズ
 */
int main(int argc, char *argv[])
{
    struct ip_mreq mreq;
    int soc;
    if (argc <= 3) {
        (void) fprintf(stderr, "u-server-m m-address port if-address\n");
        return (EX_USAGE);
    }
    // UDPマルチキャストサーバソケットの準備
    if ((soc = udp_server_socket_mcast(argv[1], argv[2], argv[3])) == -1) {
        (void) fprintf(stderr,
                        "udp_server_socket_mcast(%s, %s, %s):error\n",
                        argv[1],
                        argv[2],
                        argv[3]);
        return (EX_UNAVAILABLE);
    }
    (void) fprintf(stderr, "ready for recvfrom\n");
    // 送受信
    send_recv_loop(soc);

    // マルチキャストグループから脱退
    inet_pton(AF_INET, argv[1], &mreq.imr_multiaddr);
    inet_pton(AF_INET, argv[3], &mreq.imr_interface);
    if (setsockopt(soc,
                    IPPROTO_IP,
                    IP_DROP_MEMBERSHIP,
                    &mreq,
                    sizeof(mreq)
                    ) == -1) {
        perror("setsockopt");
        (void) close(soc);
        return (-1);
    }
    // ソケットクローズ
    (void) close(soc);
    return (EX_OK);
}
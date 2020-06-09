/**
 * 9.3 UDP/IPサーバ
 * 
 * ポート番号をbind()で固定し、複数のクライアントから要求を受付、結果を応答するサーバプログラム
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
 * 受信準備
 * 
 * UDP/IPのパケットを受信する準備は大抵の場合どのUDP/IPサーバでも同様
 * なのでTCP/IPのserver_socket()のように関数化しておくと便利
 */

/**
 * UDPサーバソケットの準備
 */
int udp_server_socket(const char *portnm)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int soc, opt, errcode;
    socklen_t opt_len;

    // アドレス情報のヒントをゼロクリア
    (void) memset(&hints, 0, sizeof(hints));

    /**
     * IPアドレスを固定でbind()しないように、ai_flagsにはAI_PASSSIVEを指定し
     * getaddrinfoの第1引数にはアドレスを指定しない。いわゆるINADDR_ANY状態とする
     * 
     * ai_socktypeにはSOCK_DGRAMを指定。これがUDP/IPを利用すると言う意味。
     */
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
                                    NI_NUMERICHOST | NI_NUMERICSERV
        )) != 0) {
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
    
    /**
     * 再利用フラグはTCP/IPの時と異なる
     * 
     * TCP/IPでは再利用フラグをオンにしても1つのプロセスがバインドして使っているポートを
     * 他のプロセスはバインドできない。
     * UDP/IPではバインド中のポートでも他のプロセスがバインドできる。
     * 
     * あとで紹介するマルチキャスト(1:n)では複数プロセスが同一ポートで受信できるが、
     * ユニキャスト(1:1)では後から起動した方にのみデータが届く
     * 
     * これを知らないと同一ポートをバインドしているプロセスが複数ある時、先に起動したプログラムで接続できない
     * とバグと勘違いする。
     */
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
    freeaddrinfo(res0);
    return (soc);
}

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
 */
int main(int argc, char *argv[])
{
    int soc;
    // ポート番号指定チェック
    if (argc <= 1) {
        (void) fprintf(stderr, "u-server port \n");
        return (EX_USAGE);
    }
    // UDPサーバソケットの準備
    if ((soc = udp_server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "udp_server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }
    (void) fprintf(stderr, "ready for recvfrom\n");
    // 送受信
    send_recv_loop(soc);
    // ソケットクローズ
    (void) close(soc);
    return (EX_OK);
}

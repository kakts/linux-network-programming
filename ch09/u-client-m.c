/**
 * 9.10.2 マルチキャスト用クライアント
 * u-client.cとほぼ同じ
 * main関数でマルチキャストの設定をする
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
 * アドレス情報の取得
 * UDP/IPの場合 クライアント側はsocket()でソケットディスクリプタを生成後
 * sendto()に送信先を指定すればそのまま送信でき、recvfrom()で受信ができるため、
 * 専用関数を用意するほどのこと花い。
 * 
 * ここでは送信のたびに送信先を指定できるようにするため関数にしている。
 */
int get_sockaddr_info(const char *hostnm, const char *portnm,
                        struct sockaddr_storage *saddr,
                        socklen_t *saddr_len)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct addrinfo hints, *res0;
    int errcode;

    // アドレス情報のヒントをゼロクリア
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM; // UDP/IP

    // アドレス情報の決定
    if ((errcode = getaddrinfo(hostnm, portnm, &hints, &res0)) != 0) {
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
    (void) fprintf(stderr, "addr=%s\n", nbuf);
    (void) fprintf(stderr, "port=%s\n", sbuf);
    (void) memcpy(saddr, res0->ai_addr, res0->ai_addrlen);
    *saddr_len = res0->ai_addrlen;
    freeaddrinfo(res0);
    return (0);
}

/**
 * 送受信
 * select()でstdinとソケットを多重化し、stdin readyの場合は
 * ${hostname}:${port} 形式でstdinから読み込みアドレス情報の取得関数を使って
 * 送信先情報を取得し、sendto()で送信する。
 * 
 * UDP/IPのソケットがreadyの場合はrecvfrom()で受信し、文字列化して標準出力に表示
 */
void send_recv_loop(int soc)
{
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    char buf[512], buf2[512];
    struct sockaddr_storage from, to;
    struct timeval timeout;
    int end, errcode, width;
    ssize_t len;
    socklen_t fromlen, tolen;
    fd_set mask, ready;
    char *hostnm, *portnm;

    // select()用マスク
    FD_ZERO(&mask);
    // ソケットディスクリプタをセット
    FD_SET(soc, &mask);
    // stdinをセット
    FD_SET(0, &mask);
    width = soc + 1;

    // 送受信
    for (end = 0;;) {
        // マスクの代入
        ready = mask;
        // タイムアウト値のセット
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        switch (select(width, (fd_set *) &ready, NULL, NULL, &timeout)) {
        case -1:
            // error
            perror("select");
            break;
        case 0:
            // timeout
            break;
        default:
            // ready有り
            // ソケット ready
            if (FD_ISSET(soc, &ready)) {
                // 受信
                fromlen = sizeof(from);
                if ((len = recvfrom(
                        soc,
                        buf,
                        sizeof(buf),
                        0,
                        (struct sockaddr *) &from,
                        &fromlen)) == -1) {
                    // error
                    perror("recvfrom");
                    end = 1;
                    break;
                }

                if ((errcode = getnameinfo((struct sockaddr *) &from, fromlen,
                                                    nbuf, sizeof(nbuf),
                                                    sbuf, sizeof(sbuf),
                                                    NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
                    (void) fprintf(stderr, "getnameinfo():%s\n", gai_strerror(errcode));
                }
                (void) printf("recdvfrom:%s:%s:len=%d\n", nbuf, sbuf, (int) len);
                // 文字列化・表示
                buf[len] = '\0';
                (void) printf("> %s", buf);
            }

            // stdin ready
            if (FD_ISSET(0, &ready)) {
                // stdinから1行読み込み
                (void) fgets(buf, sizeof(buf), stdin);
                if (feof(stdin)) {
                    end = 1;
                    break;
                }
                (void) memcpy(buf2, buf, sizeof(buf2));
                /**
                 * stdinから読み取った${hostnm}:${port}形式の文字列の解析
                 * 
                 * strtok 
                 * 第1引数: 解析対象の文字列
                 * 
                 * トークン(解析対象の文字列、記号)があれば、strtok()はトークンへのポインタを返す
                 * 2回目以降の呼び出しでは第1引数はNULL
                 * 
                 * strtok呼び出しの前になぜmemcpyをしているか
                 * strtok()ははじめに指定した文字列バッファを書き換えながらトークンを切り出す。
                 * 送信する前に元の文字列を使用できるように、一度他のバッファにコピーしてから使用する
                 * 元がbuf コピー先がbu2
                 * 
                 * 呼び出しごとに違うstrtok()は、内部で状態管理している。
                 * これはマルチスレッドに対応していないので、マルチスレッドプログラムで使うと正常な動作をしなくなる。
                 * この場合はスレッドセーフ版のstrtok_r()を使うようにする。
                 */
                if ((hostnm = strtok(buf2, ":")) == NULL) {
                    (void) fprintf(stderr, "Input-error\n");
                    (void) fprintf(stderr, "host:port\n");
                    break;
                }
                if ((portnm = strtok(NULL, "\r\n")) == NULL) {
                    (void) fprintf(stderr, "Input-error\n");
                    (void) fprintf(stderr, "host:port\n");
                    break;
                }
                // サーバアドレス情報の取得
                if (get_sockaddr_info(hostnm, portnm, &to, &tolen) == -1) {
                    (void) fprintf(stderr, "get_sockaddr_info():error\n");
                    break;
                }
                // 送信
                if ((len = sendto(soc,
                                    buf,
                                    strlen(buf),
                                    0,
                                    (struct sockaddr *) &to,
                                    tolen)) == -1) {
                    // error
                    perror("sendto");
                    end = 1;
                    break;
                }
            }
            break;
        }
        if (end) {
            break;
        }
    }
}

/**
 * main
 * 
 * ソケット生成後、アドレストポートをbind()し、マルチキャスト用送信インタフェースの指定
 * と、マルチキャストTTL、マルチキャストループバックの指定を行う
 */
int main(int argc, char *argv[])
{
    struct addrinfo hints, *res0;
    int errcode, soc, opt;
    u_char opt2;

    if (argc <= 2) {
        (void) fprintf(stderr, "u-client-m bind-address bind-port\n");
        return (EX_USAGE);
    }
    // ソケットの生成
    if ((soc = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("socket");
        return (EX_UNAVAILABLE);
    }
    // ソケットオプション(再利用フラグ)設定
    opt = 1;
    if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) == -1) {
        perror("setsockopt");
        return (-1);
    }
    // IPアドレスのバインド
    // アドレス情報のヒントをゼロクリア
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    // アドレス情報の決定
    if ((errcode = getaddrinfo(argv[1], argv[2], &hints, &res0)) != 0) {
        (void) fprintf(stderr, "getaddrinfo():%s\n", gai_strerror(errcode));
        return (EX_UNAVAILABLE);
    }
    // IPアドレスのバインド
    if (bind(soc, (struct sockaddr *) res0->ai_addr, res0->ai_addrlen) == -1) {
        perror("bind");
        freeaddrinfo(res0);
        (void) close(soc);
        return (EX_UNAVAILABLE);
    }
    freeaddrinfo(res0);
    // マルチキャスト送信インタフェースの指定
    if (setsockopt(soc,
                    IPPROTO_IP,
                    IP_MULTICAST_IF,
                    &((struct sockaddr_in *) res0->ai_addr)->sin_addr,
                    sizeof(struct in_addr)) == -1) {
        perror("setsockopt");
        (void) close(soc);
        return (EX_UNAVAILABLE);
    }

    // マルチキャストTTLの指定
    opt2 = 64;
    if (setsockopt(soc,
                    IPPROTO_IP,
                    IP_MULTICAST_TTL,
                    &opt2,
                    sizeof(u_char)) == -1) {
        perror("setsockopt");
        (void) close(soc);
        return (EX_UNAVAILABLE);
    }
    // マルチキャストループバックの指定
    opt2 = 1;
    if (setsockopt(soc,
                    IPPROTO_IP,
                    IP_MULTICAST_LOOP,
                    &opt2,
                    sizeof(u_char)) == -1) {
        perror("setsockopt");
        (void) close(soc);
        return (EX_UNAVAILABLE);
    }
    // 送受信
    send_recv_loop(soc);
    // ソケットクローズ
    (void) close(soc);
    return (EX_OK);
}

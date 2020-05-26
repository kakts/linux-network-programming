/**
 * 6.3受信タイムアウト
 * ch01 server.cを元に作る
 * 
 * 起動時の引数でどのタイプのタイムアウト処理を使うかを指定できるようにした
 */
// #include <sys/epoll.h> MAC OSでは対応していない
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h> // add
#include <poll.h> // add 
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h> // add
#include <unistd.h>

/**
 * プリプロセッサ定義・グローバル変数
 * タイムアウト時間をTIMEOUT_SECという定数にして、10秒とする
 * 
 * どのタイムアウト処理を使うかをグローバル変数で保持するようにした
 */
// 受信タイムアウト時間
#define TIMEOUT_SEC (10)
/**
 * モード
 * n: ノンブロッキング
 * s: select()
 * p: poll()
 * e: EPOLL
 * i: ioctl()
 * o: setsockopt()
 */
char g_mode = ' ';

/**
 * サーバソケットの準備
 * アクセプトループ
 * 
 * ch01 server.cと同じ
 */
int server_socket(const char *portnm)
{
  char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
  struct addrinfo hints, *res0;
  int soc, opt, errcode;
  socklen_t opt_len;

  // アドレス情報のヒントをゼロクリア
  (void) memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; // IP
  hints.ai_socktype = SOCK_STREAM; // TCP
  hints.ai_flags = AI_PASSIVE; // サーバソケット

  /**
   * アドレス情報の決定
   * 
   * getaddrinfoはアドレス情報を決定するためのヒントを与えることでsockaddr型構造体を得ることができる。
   * ヒントとなる情報を格納した不完全なaddrinfo構造体を与えると、必要なメンバが全てそろったaddrinfo型構造体を返す。
   * 
   * 第4引数に、決定されたアドレス情報を格納したaddrinfo構造体のポインタを渡す。
   * getaddrinfoを呼んだ時点でアロケートされるため利用後はfreeaddrinfo()で解放する
   */
  if ((errcode = getaddrinfo(NULL, portnm, &hints, &res0)) != 0) {
    (void) fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(errcode));
    return -1;
  }

  if ((errcode = getnameinfo(res0->ai_addr, res0->ai_addrlen, nbuf, sizeof(nbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
    (void) fprintf(stderr, "getnameinfo(): %s\n", gai_strerror(errcode));
    freeaddrinfo(res0);
    return -1;
  }

  (void) fprintf(stderr, "port=%s\n", sbuf);

  /**
   * ソケットの生成
   * 
   * socket()でソケットディスクリプタを得る
   */
  if ((soc = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) == -1) {
    perror("socket");
    freeaddrinfo(res0);
    return -1;
  }

  /**
   * ソケットオプション(再利用フラグ)設定
   * 
   * 再利用フラグの設定を行わずにbind()してしまうと、クライアントの通信が中断した場合や、
   * 並列処理でクライアントとの通信が終わってしまった場合などに、同じアドレスとポートの組み合わせでbind()できなくなる
   * 
   * 試す場合はこの処理をコメントアウトして起動して、再起動するとエラーになる
   */
  opt = 1;
  opt_len = sizeof(opt);
  if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &opt, opt_len) == -1) {
    perror("setsockopt");
    (void) close(soc);
    freeaddrinfo(res0);
    return -1;
  }

  /**
   * ソケットにアドレスを指定
   * 
   * bind()でソケットにアドレスを指定する
   * sockaddr構造体を使ってアドレスを指定する
   * 
   * res0->ai_addrが sockaddr型　その構造体のサイズであるres0->ai_addrlenを指定する。
   */
  // ソケットにアドレスを指定
  if (bind(soc, res0->ai_addr, res0->ai_addrlen) == -1) {
    perror("bind");
    (void) close(soc);
    freeaddrinfo(res0);
    return -1;
  }

  /**
   * アクセスバックログの指定
   * 
   * listen()を呼び出すことで、このソケットに対するアクセスバックログ(接続待ちのキューの数)を指定する。
   * 
   * SOMAXCONNはシステムでの最大値となる linuxでは128
   * 
   * 小さい数を指定すると、接続要求(SYN)に対して、何も応答しないという現象が起きやすくなる。
   * クライアント側は応答がないのでSYNの再送を繰り返してしまう。
   * 
   * listen()を呼び出すとソケットは接続待ち受け可能な状態になる
   * socket()で作られたばかりのソケットは、待ち受け可能ではないので、listen()せずにaccept()するとエラーになる
   * 
   * listen()されたソケットに対してクライアントからの接続要求があった場合 TCPの3way handshakeが加療する
   */
  if (listen(soc, SOMAXCONN) == -1) {
    perror("listen");
    (void) close(soc);
    freeaddrinfo(res0);
    return -1;
  }

  freeaddrinfo(res0);
  return (soc);
}



/**
 * ブロッキングモードのセット
 * 
 * ノンブロッキングモードに切り替えるためのch04のclient-timeout.cでも使ったset_block()も追加しておく
 * 
 * ソケットをノンブロッキングにするには
 * fcntl()だけでなく、ioctl()を使っても可能
 */
int set_block(int fd, int flag)
{
    int flags;

    if ((flags = fcntl(fd, F_GETFL, 0)) == -1) {
        perror("fcntl");
        return (-1);
    }
    if (flag == 0) {
        // ノンブロッキング
        (void) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } else if (flag == 1) {
        // ブロッキング
        (void) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    return (0);
}

/**
 * タイムアウト付き受信(ノンブロッキング)
 * 
 * ノンブロッキングモードを使う方法。
 * 最初にset_block()でソケットをノンブロキングにし、開始時間をtime()で取得し
 * ループで現在時刻と開始時刻の差がTIMEOUT_SECを超えたら、タイムアウトとしてループを抜ける
 * 
 * ノンブロッキングのためrecv()を毎回直接呼び出し、エラーでerrnoがEAGAINの場合は受信すべきデータがないので
 * 再度recv()させるが、CPUパワー消費を抑えるために0.1秒スリープを入れる。
 * 
 * EAGAIN以外の場合はrecv()自体のエラーなのでループを抜ける
 * 
 * recv()の戻り値が0以上の場合は受信した、もしくはソケット切断なのでループを終了する
 * ループを抜けたらブロッキングモードに戻して関数を終了する。
 * 関数の戻り値はrecv()同様に、エラーは-1,切断が0,正の値が受信したサイズとする
 * 
 * サイズの大きなデータを受信する場合には、ノンブロッキングとブロッキングでは受信サイズが異なる。
 * 通常のrecv()と同じ受信サイズのままタイムアウトを行う場合はこの方法は適さない。
 */
ssize_t recv_with_timeout_by_nonblocking(int soc, char *buf, size_t bufsize, int flag)
{
    int end;
    ssize_t len, rv;
    time_t start_time;

    // ノンブロッキングモード
    set_block(soc, 0);
    start_time = time(NULL);
    do {
        end = 0;
        if (time(NULL) - start_time > TIMEOUT_SEC) {
            (void) fprintf(stderr, "Timeout\n");
            rv = -1;
            end = 1;
        }
        if ((len = recv(soc, buf, bufsize, flag)) == -1) {
            // エラー
            if (errno == EAGAIN) {
                (void) fprintf(stderr, ".");
                (void) usleep(100000);
            } else {
                perror("recv");
                rv = -1;
                end = 1;
            }
        } else {
            rv = len;
            end = 1;
        }
    } while (end == 0);

    // ブロッキングモード
    (void) set_block(soc, 1);
    return (rv);
}

/**
 * タイムアウト付き受信(select)
 * 
 * select()を使う方法
 * 
 * マスクにソケットを1つだけ指定し、TIMEOUT_SECをタイムアウト値としてselect()をコールする。
 * -1が帰った場合はエラーだが、割り込みが発生した場合は再実行すれば良いので、EINTR以外の場合のみ
 * ループを抜けるようにする。
 * 
 * ch05で説明した通り、LinuxではデフォルトでシグナルにSA_RESTARTが指定されるので、
 * シグナルに割り込まれた場合にも、自動的にselect()は再実行されEINTRとはならない。
 * 
 * 後述するpoll()やEPOLLを使った方式でも同じことが言える。
 * EINTRとなった場合は、再度同じ処理を実行する癖をつけるといい
 * 
 * 0が返った場合はタイムアウト。それ以外はFD_ISSET()でソケットがreadyになったか調べ、recv()を行う。
 * 関数の戻り値はrecv()同様に、エラーは-1,切断が0,正の値が受信したサイズとする
 */
ssize_t recv_with_timeout_by_select(int soc, char *buf, size_t bufsize, int flag)
{
    struct timeval timeout;
    fd_set mask;
    int width, end;
    ssize_t len, rv;

    // select()用マスクの作成
    FD_ZERO(&mask);
    FD_SET(soc, &mask);
    width = soc + 1;
    timeout.tv_sec = TIMEOUT_SEC;
    timeout.tv_usec = 0;

    do {
        end = 0;
        switch (select(width, &mask, NULL, NULL, &timeout)) {
        case -1:
            if (errno != EINTR) {
                perror("select");
                rv = -1;
                end = 1;
            }
            break;
        case 0:
            (void) fprintf(stderr, "Timeout\n");
            rv = -1;
            end = 1;
            break;
        default:
            if (FD_ISSET(soc, &mask)) {
                if ((len = recv(soc, buf, bufsize, flag)) == -1) {
                    perror("recv");
                    rv = -1;
                    end = 1;
                } else {
                    rv = len;
                    end = 1;
                }
            }
            break;
        }
    } while (end == 0);
    return (rv);
}

/**
 * タイムアウト付き受信(poll)
 * 
 * poll()を使う方法。
 * pollfd構造体にソケットをPOLLINで指定して、TIMEOUT_SECをタイムアウト値としてpoll()を実行する。
 * -1が返った場合はエラー。割り込みが発生した場合は再実行すればよい。EINTR以外の場合のみループを抜ける
 * 
 * 0が返った場合はタイムアウト。
 * それ以外の場合は、reventsメンバでreadyもしくはエラーになっているかどうかを調べ、recv()を行う。
 * 
 * 関数の戻り値はrecv()同様に、エラーは-1,切断が0,正の値が受信したサイズとする
 */
ssize_t recv_with_timeout_by_poll(int soc, char *buf, size_t bufsize, int flag)
{
    struct pollfd targets[1];
    int nready, end;
    ssize_t len, rv;
    // poll()用データの作成
    targets[0].fd = soc;
    targets[0].events = POLLIN;

    do {
        end = 0;
        switch ((nready = poll(targets, 1, TIMEOUT_SEC * 1000))) {
        case -1:
            if (errno != EINTR) {
                perror("poll");
                rv = -1;
                end = 1;
            }
            break;
        case 0:
            (void) fprintf(stderr, "Timeout\n");
            rv = -1;
            end = 1;
            break;
        default:
            if (targets[0].revents&(POLLIN | POLLERR)) {
                if ((len = recv(soc, buf, bufsize, flag)) == -1) {
                    perror("recv");
                    rv = -1;
                    end = 1;
                } else {
                    rv = len;
                    end = 1;
                }
            }
            break;
        }
    } while (end == 0);
    return (rv);
}

// /**
//  * タイムアウト付き受信(EPOLL)
//  * 
//  * epoll_event型構造体にソケットをEPOLLINで指定して、epoll_ctl()を実行
//  * TIMEOUT_SECをタイムアウト値としてepoll_wait()を実行する。
//  * 
//  * 0が返った場合はタイムアウトで、それ以外の場合は一応eventsメンバでreadyもしくは
//  * エラーになっているかどうか調べ、recv()を行う。
//  * 
//  */
// ssize_t recv_with_timeout_by_epoll(int soc, char *buf, size_t bufsize, int flag)
// {
//     struct epoll_event ev, event;
//     int nfds, end, epollfd;
//     ssize_t len, rv;

//     // EPOLL用データの作成
//     if ((epollfd = epoll_create(1)) == -1) {
//         perror("epoll_create");
//         return (-1);
//     }
//     ev.data.fd = soc;
//     ev.events = EPOLLIN;
//     if (epoll_ctl(epollfd, EPOLL_CTL_ADD, soc, &ev) == -1) {
//         perror("epoll_ctl");
//         (void) close(epollfd);
//         return (-1);
//     }
//     do {
//         end = 0;
//         switch ((nfds = epoll_wait(epollfd, &event, 1, TIMEOUT_SEC * 1000))) {
//         case -1:
//             if (errno != EINTR) {
//                 perror("epoll");
//                 rv = -1;
//                 end = 1;
//             }
//             break;
//         case 0:
//             (void) fprintf(stderr, "Timeout\n");
//             rv = -1;
//             end = 1;
//             break;
//         default:
//             if (event.events&(EPOLLIN | EPOLLERR)) {
//                 if ((len = recv(soc, buf, bufsize, flag)) == -1) {
//                     perror("recv");
//                     rv = -1;
//                     end = 1;
//                 } else {
//                     rv = len;
//                     end = 1;
//                 }
//             }
//             break;
//         }
//     } while (end == 0);

//     if (epoll_ctl(epollfd, EPOLL_CTL_DEL, soc, &ev) == -1) {
//         perror("epoll_ctl");
//     }
//     (void) close(epollfd);
//     return (rv);
// }

/**
 * タイムアウト付き受信(ioctl)
 * ioctl()を使う方法
 * 
 * ioctl()はデバイスドライバなどに指示を与える汎用のインタフェースで、ディスクリプタなどにも指示を出せる。
 * 
 * ioctl()でFIONREADを調べると、ディスクリプタで読み出し可能なバイト数が得られる。
 * 開始時間を保持しておき、ループで現在時間との差がTIMEOUT_SECを超えたらタイムアウトとする。
 * 
 * 読み出し可能バイト数が0より大きい場合はrecv()で受信。
 * 
 * FIONREADでは切断の場合も0が得られ、データがないのか切断されたのか判断できない。
 */
ssize_t recv_with_timeout_by_ioctl(int soc, char *buf, size_t bufsize, int flag)
{
    int end;
    ssize_t len, rv, nread;
    time_t start_time;
    start_time = time(NULL);
    do {
        end = 0;
        if (time(NULL) - start_time > TIMEOUT_SEC) {
            (void) fprintf(stderr, "Timeout\n");
            rv = -1;
            end = 1;
        } else {
            if (ioctl(soc, FIONREAD, &nread) == -1) {
                perror("ioctl");
                rv = -1;
                end = 1;
            }
            if (nread <= 0) {
                (void) fprintf(stderr, ".");
                (void) usleep(100000);
            } else {
                if ((len = recv(soc, buf, bufsize, flag)) == -1) {
                    perror("recv");
                    rv = -1;
                    end = 1;
                } else {
                    rv = len;
                    end = 1;
                }
            }
        }
    } while (end == 0);
    return (rv);
}

/**
 * タイムアウト付き受信(setsockopt)
 * 
 * setsockopt()を使う方法
 * ソケットのオプションを変更する関数。
 * 
 * SO_RCVTIMEOの値を変更すると、その時間が経過するまで1byteも受信がない場合
 * まるでノンブロッキングモードのソケットのようにEAGAINとなってタイムアウトする
 * 
 */
ssize_t recv_with_timeout_by_setsockopt(int soc, char *buf, size_t bufsize, int flag)
{
    struct timeval tv;
    int end;
    ssize_t len;
    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = 0;

    if (setsockopt(soc, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv, sizeof(tv)) == -1) {
        perror("setsockopt");
        return (-1);
    }
    do {
        end = 0;
        if ((len = recv(soc, buf, bufsize, flag)) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                (void) fprintf(stderr, "Timeout\n");
            } else {
                perror("recv");
            }
            len = -1;
            end = 1;
        } else {
            end = 1;
        }
    } while (end == 0);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (setsockopt(soc, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv, sizeof(tv)) == -1) {
        perror("setsockopt");
    }
    return (len);
}

/**
 * タイムアウト付き受信
 *　
 * 方式に応じてどの関数を呼び出すのかを振り分けているラッパー関数
 */
ssize_t recv_with_timeout(int soc, char *buf, size_t bufsize, int flag)
{
    switch (g_mode) {
    case 'n':
        return (recv_with_timeout_by_nonblocking(soc, buf, bufsize, flag));
        break;
    case 's':
        return (recv_with_timeout_by_select(soc, buf, bufsize, flag));
        break;
    case 'p':
        return (recv_with_timeout_by_poll(soc, buf, bufsize, flag));
        break;
    // case 'e':
    //     return (recv_with_timeout_by_epoll(soc, buf, bufsize, flag));
    //     break;
    case 'i':
        return (recv_with_timeout_by_ioctl(soc, buf, bufsize, flag));
        break;
    case 'o':
        return (recv_with_timeout_by_setsockopt(soc, buf, bufsize, flag));
        break;
    default:
        return (-1);
        break;
    }
    return (-1);
}


/**
 * テスト
 */
ssize_t mystrlcat(char *dst, const char *src, size_t size)
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

  while(*ps++);
  return (dlen + (ps - src - 1));
}


/**
 * 送受信ループ
 * ch01 の処理とほぼ同じ
 */
void send_recv_loop(int acc)
{
    char buf[512], *ptr;
    ssize_t len;
    for (;;) {
        // 受信
        if ((len = recv_with_timeout(acc, buf, sizeof(buf), 0)) == -1) {
            // エラー
            (void) fprintf(stderr, "recv:ERROR\n");
            break;
        }
        if (len == 0) {
            // EOF
            (void) fprintf(stderr, "recv:EOF\n");
            break;
        }
        // 文字列化・表示
        buf[len] = '\0';
        if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
            *ptr = '\0';
        }
        (void) fprintf(stderr, "[client]%s\n", buf);
        // 応答文字列作成
        (void) mystrlcat(buf, ":OL\r\n", sizeof(buf));
        len = (ssize_t) strlen(buf);
        // 応答
        if ((len = send(acc, buf, (size_t) len, 0)) == -1) {
            // エラー
            perror("send");
            break;
        }
    }
}

/**
 * アクセプトループ
 * 
 * accept()を呼び出すと、待ち受け状態から1つの接続を受け付ける。
 * 1つも待ちがない場合この関数はブロックする
 * 
 * 他の処理と多重化したい場合はselect() poll()などを利用する。
 */
void accept_loop(int soc)
{
  char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
  struct sockaddr_storage from;
  int acc;
  socklen_t len;

  for (;;) {
    len = (socklen_t) sizeof(from);

    // 接続受付
    if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
      if (errno != EINTR) {
        perror("accept");
      }
    } else {
      (void) getnameinfo((struct sockaddr *) &from, len, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV);
      (void) fprintf(stderr, "accept: %s:%s\n", hbuf, sbuf);

      // 送受信ループ
      send_recv_loop(acc);

      // アクセプトソケットのクローズ
      (void) close(acc);
      acc = 0;
    }
  }
}

/**
 * main関数
 * 
 * 第3引数でどの方式を使うのかを判断する処理を追加
 */
int main(int argc, char *argv[])
{
    int soc;
    // 引数にポートが指定されているか
    if (argc <= 2) {
        (void) fprintf(stderr, "timeout port <[N]onblocking/[S]elect/[P]oll/[E]POLL/[I]octl/setsock[O]pt>\n");
        return (EX_USAGE);
    }
    /**
     * モードオプションの追加
     */
    if (toupper(argv[2][0]) == 'N') {
        (void) fprintf(stderr, "Nonblocking mode\n");
        g_mode = 'n';
    } else if (toupper(argv[2][0]) == 'S') {
        (void) fprintf(stderr, "Select mode\n");
        g_mode = 's';
    } else if (toupper(argv[2][0]) == 'P') {
        (void) fprintf(stderr, "Poll mode\n");
        g_mode = 'p';
    // } else if (toupper(argv[2][0]) == 'E') {
    //     (void) fprintf(stderr, "EPOLL mode\n");
    //     g_mode = 'e';
    } else if (toupper(argv[2][0]) == 'I') {
        (void) fprintf(stderr, "ioctl mode\n");
        g_mode = 'i';
    } else if (toupper(argv[2][0]) == 'O') {
        (void) fprintf(stderr, "setsockopt mode\n");
        g_mode = 'o';
    } else {
        (void) fprintf(stderr, "mode error (%s)\n", argv[2]);
        return (EX_USAGE);
    }

    // サーバソケットの準備
    if ((soc = server_socket(argv[1])) == -1) {
        (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
        return (EX_UNAVAILABLE);
    }
    (void) fprintf(stderr, "ready for accept\n");
    // アクセプトループ
    accept_loop(soc);
    // ソケットクローズ
    (void) close(soc);
    return (EX_OK);
}
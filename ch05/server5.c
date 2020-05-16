/**
 * ch05
 * 5.6.1 マルチプロセスによる多重化 マルチクライアント化
 * 
 * fork()を使っって子プロセスを生成させる
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
 * 接続受付準備
 * 
 * ソケットの生成から接続待受の段階ではマルチクライアントに関する配慮は必要ない
 * ch01 server.cと同じ関数を利用
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
 * サイズ指定文字列連結
 * 
 */
size_t mystrlcat(char *dst, const char *src, size_t size)
{
    const char *ps;
    char *pd, *pde;
    size_t dlen, lest;

    for (pd = dst, lest = size; *pd != '\n' && lest != 0; pd++, lest--);
    dlen = pd - dst;
    if (size - dlen == 0) {
        return (dlen + strlen(src));
    }
    pde = dst + size - 1;
    for (ps = src; &ps != '\0' && pd < pde; pd++, ps++) {
        *pd = *ps;
    }

    for (; pd <= pde; pd++) {
        *pd = '\0';
    }
    while (*ps++);
    return (dlen + (ps - src - 1));
}

/**
 * 送受信ループ
 * ch01 server.cと同一だが、デバッグ用にプロセスID(getpid()で取得)をログ表示させる。
 */
void send_recv_loop(int acc)
{
  char buf[512], *ptr;
  ssize_t len;
  for (;;) {
    // 受信
    if ((len = recv(acc, buf, sizeof(buf), 0)) == -1) {
      // エラー
      perror("recv");
      break;
    }

    if (len == 0) {
      // end of file
      (void) fprintf(stderr, "<%d>recv:EOF\n", getpid());
      break;
    }

    // 文字列化・表示
    buf[len] = '\0';
    if ((ptr = strpbrk(buf, "\r\n")) != NULL) {
      *ptr = '\0';
    }
    (void) fprintf(stderr, "<%d>[client]%s\n", getpid(), buf);

    // 応答文字列作成
    (void) mystrlcat(buf, ":OK\r\n", sizeof(buf));
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
 * 接続受付
 * accept()で接続受付を行なった後、送受信処理のsend_recv_loop()を実行する前に
 * fork()で送受信処理専用の子プロセスを生成する。
 * 
 * ch01のserver.cと異なるのはfork()の部分とwaitpid()が追加されていることだけ
 * (マルチプロセスプログラミングのわかりやすくて良い点)
 * 
 * fork()の動作
 * 1: 子プロセスの生成 fork()を実行した時点で子プロセスが複製され、並列処理が開始される。
 * 2: 子プロセスの終了 子プロセスが_exit()した時点で終了
 * 3: 子プロセスの終了の検知 親プロセスにSIGCHLDが通知され、wait(), waitpid()で終了状態を得る。
 * 
 * fork()の戻り値
 * 0: プロセスが複製されたときの子プロセス側
 *    子プロセスにもオープンしているソケットディスクリプタは受け継がれるが、子プロセスでは
 *    接続受付は行わずクライアントの送受信を行うだけなので、サーバソケットはすぐにクローズする
 * res > 0: 親プロセス側
 *    戻り値は子プロセスのプロセスID このプロセスIDはkill()で強制終了させたりする場合や
 *    子プロセスの終了を得るために重要
 * 　　アクセプトソケットは親プロセスでは使わないのですぐにクローズして-1をセットする
 * -1: fork()の失敗 リソース不足など
 * 　 アクセプトソケットをクローズして-1をセットする
 * 
 * fork()で生成した子プロセスの終了
 * 子プロセスが終了するとSIGCHLDのシグナルを親に通知する
 * 一般的にはSIGCHLDをキャッチした際にwait()で子プロセスの終了を得るが、
 * 多数の子プロセスを生成し、ほぼ同時に終了した場合はOSによってはSIGCHLDが終了した子プロセスの個数分通知されない場合がある。
 * 
 * ゾンビプロセスについて
 * 子プロセスの終了をwait()しないで放置すると、いつまでもリソースが解放されないゾンビプロセスになる。
 * ゾンビプロセスはkillコマンドでも親プロセスが死ぬまでは完全には殺すことができないため、必ずwait()してリソースを回収する。
 * 
 * wait()は1つでも子プロセスの終了が得られないとブロックしてしまう。
 * そのため、waitpid()で第3引数にWNOHANGを指定して、ノンブロッキングで定期的に子プロセスの終了をwaitpid()する。
 * もちろんSIGCHLDでwait()も行う。
 */

/**
 * アクセプトループ
 */
void accept_loop(int soc)
{
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    struct sockaddr_storage from;
    int acc, status;
    pid_t pid;
    socklen_t len;

    for (;;) {
        // サーバソケットready
        len = (socklen_t) sizeof(from);

        // 接続受付
        if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
            if (errno != EINTR) {
                perror("accept");
            }
        } else {
            (void) getnameinfo((struct sockaddr *) &from, len,
                            hbuf, sizeof(hbuf),
                            sbuf, sizeof(sbuf),
                            NI_NUMERICHOST | NI_NUMERICSERV);
            (void) fprintf(stderr, "accept:%s:%s\n", hbuf, sbuf);
            if ((pid = fork()) == 0) {
                /**
                 * 0: プロセスが複製されたときの子プロセス側
                 *    子プロセスにもオープンしているソケットディスクリプタは受け継がれるが、子プロセスでは
                 *    接続受付は行わずクライアントの送受信を行うだけなので、サーバソケットはすぐにクローズする
                 */
                // 子プロセス
                // サーバソケットクローズ
                (void) close(soc);
                // 送受信ループ
                send_recv_loop(acc);
                // アクセプトソケットクローズ
                (void) close(acc);
                // 子プロセス終了
                _exit(1);
            } else if (pid > 0) {
                // fork()成功 親プロセス
                // アクセプトソケットクローズ
                (void) close(acc);
                acc = -1;
            } else {
                // fork()失敗
                perror("fork");
                // アクセプトソケットクローズ
                (void) close(acc);
                acc = -1;
            }

            /**
             * シグナルでキャッチできなかった子プロセスの終了のチェック
             * 一般的にはSIGCHLDをキャッチした際にwait()で子プロセスの終了を得るが、
             * 多数の子プロセスを生成し、ほぼ同時に終了した場合はOSによってはSIGCHLDが終了した子プロセスの個数分通知されない場合がある。
             */
            if ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                // 子プロセス終了あり
                (void) fprintf(stderr, "accept_loop:waitpid:pid=%d, status=%d\n", pid, status);
                (void) fprintf(stderr,
                                " WIFEXITED: %d, WEXITSSTATUS:%d, WIFSIGNALED:%d,"
                                "WTERMSIG:%d, WIFSTOPPED:%d, WSTOPSIG:%d\n",
                                WIFEXITED(status),
                                WEXITSTATUS(status),
                                WIFSIGNALED(status),
                                WTERMSIG(status),
                                WIFSTOPPED(status),
                                WSTOPSIG(status));
            }
        }
    }
}


/**
 * 子プロセス終了シグナルハンドラ
 * 
 * 子プロセス終了時に親プロセスにSIGCHLDシグナルが届く
 * 一般的に子プロセス終了時にはwait()で子プロセスの終了状態を得る必要がある。
 * wait()しないで放置すると、親プロセスが終了するまで子プロセスがゾンビ状態になりリソース解放されない。
 * 
 * OSによってはSIGCHLDが終了した数分通知されないケースがあり、wait()だけでは不十分な場合があrう。
 * 
 * シグナルハンドラ内では fprintf()などの非同期シグナルに非安全とされる関数は使うべきでは無いが、動作の理解のために使っている。
 * 
 * この関数がコールされた際には1つ以上は子プロセスが終了しているため、wait()で終了状態を得る。
 * wait()の戻り値は終了した子プロセスのプロセスID
 * 引数で指定されたアドレスに終了状態が格納される。
 */
void sig_chld_handler(int sig)
{
    int status;
    pid_t pid;
    /**
     * 子プロセスの終了を待つ
     */
    pid = wait(&status);
    (void) fprintf(stderr, "sig_chld_handler: wait:pid=%d, status=%d\n", pid, status);
    (void) fprintf(stderr,
                    " WIFEXITED: %d, WEXITSSTATUS:%d, WIFSIGNALED:%d,"
                    "WTERMSIG:%d, WIFSTOPPED:%d, WSTOPSIG:%d\n",
                    WIFEXITED(status),
                    WEXITSTATUS(status),
                    WIFSIGNALED(status),
                    WTERMSIG(status),
                    WIFSTOPPED(status),
                    WSTOPSIG(status));
}

/**
 * main
 * 
 * ch01 server.cとほぼ同じ
 * 子プロセス終了シグナルのセットが追加されている。
 */
int main(int argc, char *argv[])
{
    int soc;

    // 引数にポート番号が指定されているか?
    if (argc <= 1) {
        (void) fprintf(stderr, "server5 port\n");
        return (EX_USAGE);
    }

    // 子プロセス終了シグナルのセット
    (void) signal(SIGCHLD, sig_chld_handler);

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


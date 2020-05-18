/**
 * ch05
 * 5.7.1 マルチプロセスによる多重化 子プロセスのプリフォーク型
 * 
 * サーバソケットを準備してから、子プロセスをあらかじめ決めた数だけ起動し、排他処理を
 * 行いながらアクセプトさせ、送受信処理を行わせる。
 * 
 * マルチプロセスのポイントは、排他をどのように行うか。
 * IPCのセマフォを使う方法や、flock(), lockf(), fcntl()など様々な方法がある
 * ここでは一番汎用的でわかりやすいlockf()を使ったファイルでのロックを使う
 * 
 * lockf()は内部でfcntl()を呼び出している。直接fcntl()を使っての排他と同等。
 * flock()はNFSで使えず、古い感じがある。
 */
#include <sys/file.h> // 追加
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
 * プリプロセッサ定義・グローバル変数
 * 
 * 子プロセスの数とロックファイルのパスを定義する。
 * ここではオーバーした場合のテストがしやすいように子プロセス数は2にしている。
 */
#define NUM_CHILD 2
#define LOCK_FILE "./server7.lock"

/**
 * ロックファイルのfdをグローバルで定義。
 * 
 * これまでのサンプルと似たような状態にしておくのと、
 * 将来的にシグナル処理をきちんと作るときに便利なためグローバルにしておく。
 **/
int g_lock_fd = -1;

/**
 * 接続受付
 * ch01 server.cとほぼ同じ
 * アクセプトの前に排他処理のためロック獲得を行う。
 * lockf()は第2引数にF_LOCKを指定すると、ロックが獲得できるまで待つ。
 * 
 * ロックが獲得できた場合にアクセプトをおこない、ロックを解放する。
 * ロックの解放はlockf()の第2引数にF_ULOCKを指定する。
 * 
 * 送受信処理を行なった後、アクセプトソケットをクローズし、またロック獲得待ちになる。
 */

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
 * アクセプトループ
 */
void accept_loop(int soc)
{
  char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
  struct sockaddr_storage from;
  int acc;
  socklen_t len;

  for (;;) {
    /**
     * ロック獲得
     */
    (void) fprintf(stderr, "<%d>ロック獲得開始\n", getpid());
    (void) lockf(g_lock_fd, F_LOCK, 0);
    (void) fprintf(stderr, "<%d>ロック獲得! \n", getpid());

    len = (socklen_t) sizeof(from);
    /**
     * 接続受付
     */
    if ((acc = accept(soc, (struct sockaddr *) &from, &len)) == -1) {
      if (errno != EINTR) {
        perror("accept");
      }
      (void) fprintf(stderr, "<%d>ロック解放\n", getpid());
      // ロック解放
      (void) lockf(g_lock_fd, F_ULOCK, 0);
    } else {
      (void) getnameinfo((struct sockaddr *) &from, len,
                                    hbuf, sizeof(hbuf),
                                    sbuf, sizeof(sbuf),
                                    NI_NUMERICHOST | NI_NUMERICSERV);
      (void) fprintf(stderr, "<%d>accept:%s:%s\n", getpid(), hbuf, sbuf);
      (void) fprintf(stderr, "<%d>ロック解放\n", getpid());
      // ロック解放
      (void) lockf(g_lock_fd, F_ULOCK, 0);
      // 送受信ループ
      send_recv_loop(acc);
      // アクセプトソケットクローズ
      (void) close(acc);
    }
  }
}

/**
 * main
 * 
 * 今までと違い、main()が結構変わる。
 * サーバソケットを準備した後、ロックファイルの生成を行い、子プロセスを所定の個数起動する。
 * 
 * それぞれの子プロセスでは接続受付処理を行わせる。親プロセスはその後は何も行わないのでpause()で無限に待たせればよい。
 * ここではロックファイルの状態を10秒おきに表示させている。
 */
int main(int argc, char *argv[])
{
  int i, soc;
  pid_t pid;

  // 引数にポートが指定されているか
  if (argc <= 1) {
    (void) fprintf(stderr, "server7 port\n");
    return (EX_USAGE);
  }

  // サーバソケットの準備
  if ((soc = server_socket(argv[1])) == -1) {
    (void) fprintf(stderr, "server_socket(%s):error\n", argv[1]);
    return (EX_UNAVAILABLE);
  }

  // ロックファイルの生成
  if ((g_lock_fd = open(LOCK_FILE, O_RDWR | O_CREAT, 0666)) == -1) {
    perror("open");
    return (EX_UNAVAILABLE);
  }

  /**
   * ファイル名の削除
   * 生成後unlink()で見かけ上削除する
   * 
   * このプログラム内でディスクリプタが使えれば良いだけ。
   * 削除忘れ防止と・邪魔にならないように消している。
   * 
   * Unix系OSではunlink()されてもディスクリプタを使用している間はファイルの実体は削除せず、
   * 全てのディスクリプタがクローズされると本当に削除される。
   */
  (void) unlink(LOCK_FILE);
  (void) fprintf(stderr, "start %d children\n", NUM_CHILD);

  /**
   * 子プロセスを指定した個数生成
   * 
   * 子プロセスの生成は毎回同様fork()で行う。
   * 戻り値が0の場合は子プロセスなので接続受付処理を行う。
   * 
   * 接続受付処理は無限ループのため、_exit(1)はこのサンプルでは実行されない。
   */
  for (i = 0; i < NUM_CHILD; i++) {
    if ((pid = fork()) == 0) {
      /**
       * 子プロセス
       * アクセプトループ
       */
      accept_loop(soc);
      _exit(1);
    } else if (pid > 0) {
      // fork()成功: 親プロセス
    } else {
      // fork()失敗
      perror("fork");
    }
  }
  (void) fprintf(stderr, "ready for accept\n");
  /**
   * 待ち・ロック状態の表示
   * 
   * 子プロセス生成後、親プロセスは何もすることがないので、sleep()で10秒おきにロックファイルの状態を表示する
   * lockf()の第2引数にF_TESTを渡すことで状態を取得できる。
   * 
   * 無限ループなのでこの先のclose()は実行されない。
   */
  for (;;) {
    (void) sleep(10);
    (void) fprintf(stderr, "<<%d>>ロック状態: %d\n", getpid(), lockf(g_lock_fd, F_TEST, 0));
  }
  // ソケットクローズ
  (void) close(soc);
  (void) close(g_lock_fd);

  return (EX_OK);
}
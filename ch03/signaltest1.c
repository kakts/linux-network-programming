/**
 * 3.7.1 正常終了
 * シグナルに対して、終了フラグを立てるハンドラを設定する
 * プログラム内のループでそのフラグを参照し終了する
 */
#include <signal.h>
#include <stdio.h>
#include <sysexits.h>
#include <unistd.h>

/**
 * グローバル変数
 * 
 * シグナルハンドラで立てるためのフラグをグローバル変数として定義する
 * 
 * シグナルは非同期(処理の流れとは無関係)に飛んでくる
 * ハンドラ内では関数の呼び出しや変数の読み書きに制限がある
 * 
 * グローバル変数として定義されている文字列をハンドラ内で扱うと、メインの処理でまだ文字列を書き込んでる最中にシグナルが飛ぶ
 * それによってハンドラ内で期待するデータとならないことが考えられる。
 * 
 * volatile sig_atomic_tで宣言された変数は0-127の範囲の数値でシグナルハンドラで安全に扱える
 * 
 * ここではvolatile sig_atomic_tと宣言されたg_gotsigに受け取ったシグナル番号を入れる。これを終了フラグとして扱う
 */
volatile sig_atomic_t g_gotsig = 0;

/**
 * SIGINT ハンドラ
 * Ctrl + CでSIGINTを受け取ったときの動作
 * シグナルをグローバル変数に代入するのみ
 */
void sig_int_handler(int sig)
{
    g_gotsig = sig;
}

/**
 * main関数
 * 最初にシグナルハンドラを設定して、ループ内で1秒ごとに.を表示させる
 * シグナルを受け取ったらメッセージを表示して終了
 */
int main(int argc, char *argv[])
{
    struct sigaction sa;
    (void) sigaction(SIGINT, (struct sigaction *) NULL, &sa);
    sa.sa_handler = sig_int_handler;
    sa.sa_flags = SA_NODEFER;
    (void) sigaction(SIGINT, &sa, (struct sigaction *) NULL);

    /**
     * ループ シグナルを受け取っていない間は1秒ごとに.を表示
     */
    while (g_gotsig == 0) {
        (void) fprintf(stderr, ".");
        (void) sleep(1);
    }
    // シグナルを受け取った場合
    (void) fprintf(stderr, "\nEND\n");
    return (EX_OK);
}

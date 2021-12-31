#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/time.h>
#include "mytime.h"
#include <locale.h>
#include <sys/stat.h>
#include <signal.h>
//#include <errno.h>
#include "image.h"
#include <jpeglib.h>
#include <setjmp.h>
#include "bitmapfont.h"
#include "mem.h"
#include <pthread.h>

typedef enum {
    TERMINATED      = 2,
    COMPLETED       = 1,
    DISP_HELP       = 0,
    CODE_NONE       = 0,
    ARG_ERR         = -1,
    INTERNAL_ERR    = -2,
    CONNECTION_ERR  = -3,
    CONTENT_ERR     = -4,
    FILE_ERR        = -5,
    STREAM_ERR      = -6,
    MEMORY_ERR      = -7,
    LIBJPEG_ERR     = -8,
    MEMORYLOCK_ERR  = -9,
    THREAD_ERR      = -10,
    MUTEX_ERR       = -11
} RETURN_CODE;

#define BOUNDARY_BUF_SIZE   128

RETURN_CODE main_process();
void *file_write_thread();
bool begin_connection(RETURN_CODE *rc);
bool get_boundary(char *buf, int size, RETURN_CODE *rc);
char random_char();
bool file_exists(const char* filename);
void mkstemp2(char *path);
FILE *create_open_tmp_file(const char *dir, const char *name, char *buf, RETURN_CODE *rc);
bool process_image(MYMEMORY *buf, bool debug, RETURN_CODE *rc);
char *generate_timestamp_chars(time_t now);
typedef enum {
    NOT_DRAW,
    LEFT_TOP,
    RIGHT_TOP,
    RIGHT_BOTTOM,
    LEFT_BOTTOM
} DRAWTEXTPOSITION;
void draw_text(image_t *img, char *text, DRAWTEXTPOSITION pos);
char* itoa(int value, char* result, int base);
int proc_args(int argc, char *argv[]);
char *move_to_end(char *p);
void create_output_filename(char *dst, time_t tm_start, time_t tm_end, int read_frame_count, int file_number);
bool copy_dir(char *dst, int buflen, char *src);
//int fcopy(const char *fnamer, const char *fnamew);
int read_char();
void push_char(char c);
int skip_until_content_start();
bool skip_until(const char *word, int limit);
bool read_to_mem(MYMEMORY *pMem, int size);
bool read_to_throwaway(int size);
time_t get_next_boder_time(int border_minute);

static char output_dir[1024];       ///tmp/ など
static char output_name[256];       //拡張子なし。%Y %M %D %H %I %S などが使える
static char target_host[256];       //192.168.0.26 など
static int opt_F = -1;              //１ファイルに保存するフレーム枚数
static int opt_T = -1;              //１ファイルに保存する秒数
static int opt_V = -1;              //ファイルへの書き出しを時刻の分に紐づけ
static int opt_N = -1;              //保存するファイル数
static int opt_I = 1;               //開始ファイル番号を指定。 %n で使用
static int opt_S = -1;              //フレームを間引く。フレームopt_S回に1回だけ保存
static int opt_R = -1;              //書き出すフレームレート
static char opt_u[256] = "/?action=stream"; //URI
static int opt_p = 8080;            //ポート番号
static DRAWTEXTPOSITION opt_t = NOT_DRAW;//タイムスタンプ表示位置 
static char opt_X[32] = "";         //表示テキスト
static DRAWTEXTPOSITION opt_x = LEFT_TOP;//テキスト表示位置 
static int opt_q = 75;              //jpeg出力品質
static char opt_m[1024] = "";       //サムネイルパス
static int opt_i = 0;               //入力データを調査する秒数
static int opt_c = 0;               //時計回りの回転角度 ROTATE_CLOCKWISE_xx の値を格納して使用

static int sofd = 0;
static struct hostent     *shost;
static struct sockaddr_in sv_addr;

static char boundary[BOUNDARY_BUF_SIZE + 6 + 2 + 1];

MYMEMORY jpg_buffer[2];
int writing_to_buf_index = -1;
volatile int writing_to_file_index = -1;

//pthread_mutex_t jpg_buffer_mutex[2];
pthread_t thread = 0;
volatile bool thread_running = false;
RETURN_CODE thread_rc = CODE_NONE;

volatile bool new_frame_available = false;

/* SIGINTハンドラ */
volatile sig_atomic_t e_flag = 0;
void abrt_handler(int sig) { e_flag = 1; }


int main (int argc, char *argv[]) {

    //引数を処理
    int i = proc_args(argc, argv);
    if (i == 0) return DISP_HELP;
    if (i == -1) return ARG_ERR;

    //強制終了を処理するハンドラを設定 (ハンドラではe_flag = 1を実行) 
    if (signal(SIGINT, abrt_handler) == SIG_ERR) {
        fprintf(stderr, "%s: can not set SIGINT handler.\n", opt_X);
        return INTERNAL_ERR;
    }
    if (signal(SIGTERM, abrt_handler) == SIG_ERR) {
        fprintf(stderr, "%s: can not set SIGTERM handler.\n", opt_X);
        return INTERNAL_ERR;
    }

    //メインの処理
    RETURN_CODE rc = main_process();
    
    //後始末
    if (sofd) close(sofd);

    return rc;
}

RETURN_CODE main_process() {
    RETURN_CODE rc;

    //ソケットの作成～接続
    if (!begin_connection(&rc)) {
        return rc;
    }

    //boundaryの定義まで読み飛ばす
    if (!skip_until(";boundary=", 2000)) {
		fprintf(stderr, "%s: err boundary identification is not found.\n", opt_X);
		return CONTENT_ERR;
    }

    //boundary値を取得する
    if (!get_boundary(boundary, BOUNDARY_BUF_SIZE, &rc)) {
        return rc;
    }

    //最初のboundaryまで読み飛ばす
    if (!skip_until(boundary, 1000)) {
		fprintf(stderr, "%s: err can not reach start of contents.\n", opt_X);
		return CONTENT_ERR;
    }

    if (opt_i == 0) {
        //ファイル書き込みスレッドを開始する
        if (pthread_create(&thread, NULL, (void *)file_write_thread, NULL) != 0) {
            fprintf(stderr, "%s: err can not start thread.\n", opt_X);
            return THREAD_ERR;
        }
    }

    //-iオプション用
    int receive_frame_count = 0;                //受信したトータルのフレーム数
    time_t tm_start = time(NULL);
    time_t end_time = (opt_i > 0) ? tm_start + opt_i : 0; //計測終了予定
    struct timespec start_clock;
    get_current_clock(&start_clock);


    bool first_frame = true;        //１度の起動で最初のフレームだけ
    if (opt_S <= 0) opt_S = 1;
    int skip_frame = opt_S;
    bool error_happend = false;
    int locked_index = -1;          //バッファロック中は0または1を指す
    init_memory_struct(&jpg_buffer[0]);
    init_memory_struct(&jpg_buffer[1]);
    rc = TERMINATED;
    while (!e_flag) {

        //\r\n\r\nまで読み飛ばし
        int content_length = skip_until_content_start();
        if (content_length < 0) {
            fprintf(stderr, "%s: err can not get contents.\n", opt_X);
            error_happend = true;
            rc = STREAM_ERR;
            break;
        }

        //Content-Lengthを取得できない
        if (content_length == 0) {
            fprintf(stderr, "%s: err can not get content-length.\n", opt_X);
            error_happend = true;
            rc = STREAM_ERR;
            break;
        }

        if (first_frame) {
            //jpeg処理バッファを最初のフレームの２倍のサイズで確保する
            int alloc_size = (content_length >= 5000) ? content_length * 2 : 10000;
            if (!alloc_memory(&jpg_buffer[0], alloc_size, ALLOC_NEW, false) ||
                !alloc_memory(&jpg_buffer[1], alloc_size, ALLOC_NEW, false)) {
                
                fprintf(stderr, "%s: err can not allocate %d bytes of memory.\n",
                    opt_X, alloc_size);
                error_happend = true;
                rc = MEMORY_ERR;
                break;
            }
            writing_to_buf_index = 0;
            writing_to_file_index = -1;

            //デバッグ
            fprintf(stderr, "%s: content-length: %d / allocate-size:%d\n",
                opt_X, content_length, alloc_size);
        }

        if (--skip_frame == 0) {
            skip_frame = opt_S;

            //書き出し用のバッファをロックする
            if (opt_i == 0) {
                locked_index = writing_to_buf_index;
                if (!lock_memory(&jpg_buffer[locked_index])) {
                    error_happend = true;
                    rc = MEMORYLOCK_ERR;
                    break;
                }
            } else {
                locked_index = writing_to_buf_index;
            }

            //画像データをバッファに読み込む
            if (!read_to_mem(&jpg_buffer[locked_index], content_length)) {
                fprintf(stderr, "%s: err can not get contents.\n", opt_X);
                error_happend = true;
                rc = STREAM_ERR;
                break;
            }

            //タイムスタンプ書き込みや回転など、必要な場合のみ
            //jpg_buffer[locked_index]内のjpeg画像を更新する
            if (opt_c != ROTATE_CLOCKWISE_0 || opt_t != NOT_DRAW ||
                (opt_X[0] != '\0' && opt_x != NOT_DRAW) ||
                (opt_i > 0 && first_frame)) {
                
                //文字列やタイムスタンプを書き込む
                if (!process_image(&jpg_buffer[locked_index], first_frame, &rc)) {
                    error_happend = true;
                    break;
                }

                //デバッグ
                if (first_frame && opt_i == 0) {
                    fprintf(stderr, "%s: generated content-length: %d\n",
                        opt_X, jpg_buffer[locked_index].active_size);
                }
            }

            //バッファをアンロック
            if (opt_i == 0) {
                unlock_memory(&jpg_buffer[locked_index]);
            }

            //バッファ入れ替え
            writing_to_file_index = locked_index;
            writing_to_buf_index = ++locked_index & 1;
            locked_index = -1;
            new_frame_available = true;

            if (opt_i != 0) {
                ++receive_frame_count;

                //指定された秒数が経過した
                if (time(NULL) >= end_time) {
                    rc = COMPLETED;
                    break;
                }
            }
        } else {
            //スキップフレームなので画像データを読み飛ばす
            if (!read_to_throwaway(content_length)) {
                fprintf(stderr, "%s: err can not get contents.\n", opt_X);
                error_happend = true;
                rc = STREAM_ERR;
                break;
            }
        }

        first_frame = false;
        if (opt_i == 0 && !thread_running) {
            //スレッドが停止した
            rc = TERMINATED;
            break;
        }
    }

    //アンロック
    if (opt_i == 0 && locked_index >= 0) {
        unlock_memory(&jpg_buffer[locked_index]);
    }
    locked_index = -1;

    if (opt_i == 0) {
        //スレッド終了待ち(既に終わっている場合もある)
        e_flag = 1;
        pthread_join(thread, NULL);

    } else if(!error_happend) {
        //-iオプション時
        double elapsed = elapsed_clock(&start_clock);
        fprintf(stderr, "running time: %.2lf sec\n", elapsed);
        fprintf(stderr, "received frames: %d\n", receive_frame_count);
        fprintf(stderr, "frame rate: %.2lf\n", (double)receive_frame_count / elapsed);
    }

    free_memory(&jpg_buffer[0]);
    free_memory(&jpg_buffer[1]);

    if (opt_i == 0) {
        fprintf(stderr, "Return code main: %d thread: %d\n", rc, thread_rc);
    } else {
        fprintf(stderr, "Return code: %d\n", rc);
    }
    return rc;
}

//サムネイル作成
bool create_thumbnail(const char *dir, char *path, MYMEMORY *buf, RETURN_CODE *rc) {
    FILE *thumbFp = create_open_tmp_file(dir, "__thumb_XXXXXX", path, rc);
    if (thumbFp == NULL) return false;

    bool res = save_memory_to_fp(buf, thumbFp);
    fclose(thumbFp);
    if (res) return true;

    fprintf(stderr, "%s: err can not write target size %d bytes to thumbnail file.\n",
        opt_X, buf->active_size);
    *rc = FILE_ERR;
    return false;
}

bool rename_file(const char *src_path, const char *dst_path) {
    //元ファイルの権限を変更 600=>666 してPythonから触れるようにする
    if (chmod(src_path, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH) != 0) {
        fprintf(stderr, "%s: err can not modify auth of thumbnail file '%s'.\n",
            opt_X, src_path);
        return false;
    }
    //ファイル名を.jpgに変更
    if (rename(src_path, dst_path) != 0) {
        fprintf(stderr, "%s: err can not rename thumbnail file '%s' to '%s'.\n",
            opt_X, src_path, dst_path);
        return false;
    }
    return true;
}

//ファイル書き出しスレッド
//opt_i != 0 の場合は呼ばないこと
void *file_write_thread() {
    if (opt_i != 0) return 0;
    thread_running = true;

    //opt_R使用時
    int frame_count_in_sec = 0;
    struct timespec next_clock;
    long nsec_step0, nsec_step;
    if (opt_R > 0) {
        nsec_step = 1000000000 / opt_R;
        nsec_step0 = 1000000000 - nsec_step * (opt_R - 1);
        //fprintf(stderr, "opt_R=%d\n",opt_R);
        //fprintf(stderr, "nsec_step=%ld\n",nsec_step);
        //fprintf(stderr, "nsec_step0=%ld\n",nsec_step0);
    }

    //最初のフレーム待ち
    while (!new_frame_available) {
        if (e_flag) {
            thread_rc = TERMINATED;
            thread_running = false;
            return 0;
        }
    }
    new_frame_available = false;

    if (opt_R > 0) {
        //次のフレーム時間を計算
        get_future_clock(&next_clock, 0,
            (frame_count_in_sec == 0) ? nsec_step0 : nsec_step);
        if (++frame_count_in_sec == opt_R)
            frame_count_in_sec = 0;
    }

    char tmp_path[256];
    char tmp_path_thumb[256];

    while (!e_flag) {
        FILE *fp;
        bool first_frame = false;   //ファイル毎にリセット
        int write_frame_count = 0;  //ファイル毎にリセット

        //mjpg書き込み用のファイルを開く
        fp = create_open_tmp_file(output_dir, "__work_XXXXXX", tmp_path, &thread_rc);
        if (fp == NULL) {
            thread_rc = FILE_ERR;
            thread_running = false;
            return 0;
        }

        //サムネイルファイルは未定
        tmp_path_thumb[0] = '\0';

        time_t tm_start = time(NULL);

        //終了が時間指定の場合の終了時間を計算
        time_t end_time;
        struct tm *ts = NULL;
        if (opt_T > 0) {
            end_time = tm_start + opt_T;
            //デバッグ
            ts = localtime(&end_time);
        } else if (opt_V > 0) {
            end_time = get_next_boder_time(opt_V);
            //デバッグ
            ts = localtime(&end_time);
        }
        if (ts) fprintf(stderr, "%s: next file output at %d:%02d:%02d\n",
                    opt_X, ts->tm_hour, ts->tm_min, ts->tm_sec);

        thread_rc = TERMINATED;
        bool error_happend = false;
        int locked_index = -1;      //バッファロック中は0または1を指す

        while (!e_flag) {

            //読み出し用のバッファをロックする
            locked_index = writing_to_file_index;
            if (!lock_memory(&jpg_buffer[locked_index])) {
                error_happend = true;
                thread_rc = MEMORYLOCK_ERR;
                break;
            }

            //画像データをファイルに書き出す
            fputs(&boundary[(first_frame == 0) ? 4 : 2], fp);
            fputs("Content-Type: image/jpeg\r\n", fp);
            fprintf(fp, "Content-Length: %d\r\n", jpg_buffer[locked_index].active_size);
            fputs("X-Timestamp: 0.000000\r\n\r\n", fp);
            if (!save_memory_to_fp(&jpg_buffer[locked_index], fp)) {
                fprintf(stderr, "%s: err can not write target size %d bytes to mjpg file.\n",
                    opt_X, jpg_buffer[locked_index].active_size);
                error_happend = true;
                thread_rc = FILE_ERR;
                break;
            }

            //サムネイル作成
            if (first_frame && opt_m[0] != '\0') {
                if (!create_thumbnail(output_dir,
                    tmp_path_thumb, &jpg_buffer[locked_index], &thread_rc)) {

                    error_happend = true;
                    break;
                }
            }

            //アンロック
            unlock_memory(&jpg_buffer[locked_index]);
            locked_index = -1;

            //指定されたフレーム数を保存した
            ++write_frame_count;
            if (opt_F > 0 && write_frame_count >= opt_F) break;

            first_frame = false;

            if (opt_R > 0) {
                //次のフレーム時間まで待つ
                /*while (!e_flag) {
                    if (clock_passed(&next_clock)) break;

                    //指定された秒数が経過した
                    if ((opt_T > 0 || opt_V > 0) && time(NULL) >= end_time)
                        break;
                }*/
                while (!clock_passed(&next_clock))
                    usleep(10000);

                //次のフレーム時間を計算
                add_clock(&next_clock, 0,
                    (frame_count_in_sec == 0) ? nsec_step0 : nsec_step);
                if (++frame_count_in_sec == opt_R)
                    frame_count_in_sec = 0;
            } else {
                //フレームが更新されるまで待つ
                while (!e_flag && !new_frame_available) {

                    //指定された秒数が経過した
                    if ((opt_T > 0 || opt_V > 0) && time(NULL) >= end_time)
                        break;
                }
                new_frame_available = false;
            }

            //指定された秒数が経過した
            if ((opt_T > 0 || opt_V > 0) && time(NULL) >= end_time)
                break;
        }

        //上記ループ内でエラーがあっても、ファイルが空でなければきりのいい状態に処理する

        //アンロック
        if (locked_index >= 0) {
            unlock_memory(&jpg_buffer[locked_index]);
            locked_index = -1;
        }

        time_t tm_end = time(NULL);

        //mjpgファイルサイズを取得
        long mjpg_file_size = ftell(fp);
        fclose(fp);

        //サムネイル画像の処理
        char correct_path[256];
        if (tmp_path_thumb[0] != '\0' && file_exists(tmp_path_thumb)) {
            
            //正規のファイル名を作成
            strcpy(correct_path, opt_m);
            create_output_filename(move_to_end(correct_path),
                tm_start, tm_end, write_frame_count, opt_I);
            strcat(correct_path, ".jpg");

            //ファイル属性の変更とファイル名を変更
            if (!rename_file(tmp_path_thumb, correct_path)) {
                if (!error_happend) {
                    error_happend = true;
                    thread_rc = FILE_ERR;
                }
            } else {
                tmp_path_thumb[0] = '\0';
            }
        }

        //mjpgファイルの処理
        if (mjpg_file_size > 1000) {

            //正規のファイル名を作成
            strcpy(correct_path, output_dir);
            create_output_filename(move_to_end(correct_path),
                tm_start, tm_end, write_frame_count, opt_I);
            strcat(correct_path, ".mjpg");

            //ファイル属性の変更とファイル名を変更
            if (!rename_file(tmp_path, correct_path)) {
                if (!error_happend) {
                    error_happend = true;
                    thread_rc = FILE_ERR;
                }
            } else {
                tmp_path[0] = '\0';
            }

            fprintf(stderr, "%s: mjpg file created. %s\n", opt_X, correct_path);
            fprintf(stderr, "%s: %ld bytes/%d frames/%.0lf seconds\n",
                opt_X, mjpg_file_size, write_frame_count,
                elapsed_time(tm_start, tm_end, 's'));
        } else {
            fprintf(stderr, "%s: mjpg file not create due to too small size.\n",
                opt_X);
        }

        if (tmp_path[0] != '\0' && file_exists(tmp_path))
            remove(tmp_path);
        if (tmp_path_thumb[0] != '\0' && file_exists(tmp_path_thumb))
            remove(tmp_path_thumb);

        if (error_happend) break;

        //既定のファイル数に保存した
        if (opt_N > 0) {
            if (--opt_N == 0) {
                thread_rc = COMPLETED;
                break;
            }
        }

        ++opt_I;
    }
    thread_running = false;
    return 0;
}

//ソケットの作成～接続まで
bool begin_connection(RETURN_CODE *rc) {
    
    //ソケットを作成
    sofd = socket(AF_INET, SOCK_STREAM, 0);
    if (sofd < 0) {
        fprintf(stderr, "%s: can not open SOCKET.\n", opt_X);
        *rc = CONNECTION_ERR;
        sofd = 0;
        return false;
    }

    //アドレスを定義
    shost = gethostbyname(target_host);
    if (shost == NULL) {
        fprintf(stderr, "%s: err happend in gethostbyname function.\n", opt_X);
        *rc = CONNECTION_ERR;
        sofd = 0;
        return false;
    }

    memset(&sv_addr, 0, sizeof(sv_addr));
    sv_addr.sin_family = AF_INET;
    sv_addr.sin_port   = htons(opt_p);
    memcpy((char *)&sv_addr.sin_addr, (char *)shost->h_addr, shost->h_length);

    //コネクトする
    if (connect(sofd, (const struct sockaddr*)&sv_addr, sizeof(sv_addr)) < 0) {
        fprintf(stderr, "%s: err happend in connect function.\n", opt_X);
        *rc = CONNECTION_ERR;
        sofd = 0;
        return false;
    }

    //HTTPのやりとり
    send(sofd, "GET ",       4,                   0);
    send(sofd, opt_u,        strlen(opt_u),       0);
    send(sofd, " HTTP/1.0",  9,                   0);
    send(sofd, "\r\n",       2,                   0);
    send(sofd, "Host: ",     6,                   0);
    send(sofd, target_host,  strlen(target_host), 0);
    send(sofd, "\r\n\r\n",   4,                   0);
    return true;
}

bool get_boundary(char *buf, int size, RETURN_CODE *rc) {
    strcpy(buf, "\r\n\r\n--");
    buf += 6;
    int n = 0;
    char c;
    while (1) {
        c = read_char();
        if (c == -1) {
            *rc = CONNECTION_ERR;  //データなし
            return false;
        }
        if (c == '\r') {
            push_char(c);
            break;
        }
        *buf++ = c;
        if (--size <= 0) {
		    fprintf(stderr, "%s: err boundary length too long.\n", opt_X);
            *rc = CONTENT_ERR;
            return false;
        }
    }
    strcpy(buf, "\r\n");
    return true;
}

char random_char() {
    static bool init = false;
    if (!init) {
        struct timeval myTime;
        gettimeofday(&myTime, NULL);
        srand((unsigned int)myTime.tv_usec);
        init = true;
    }

    int r = rand() & 63;
    if (r < 26) return 'A' + r;
    r -= 26;
    if (r < 26) return 'a' + r;
    r -= 26;
    if (r < 10) return '0' + r;
    return (r == 0) ? '-' : '_';
}

bool file_exists(const char* filename) {
    struct stat buffer;
    return (stat(filename, &buffer) == 0);
}

//パスの末尾に連続する'X'をファイル名の重複しない任意の文字に置き換えて返す
void mkstemp2(char *path) {
    if (path == NULL) return;
    int l = strlen(path);
    if (l == 0) return;
    int lx = 0;
    char *p = path;
    while (*p != '\0') {
        lx = (*p == 'X') ? lx + 1 : 0;
        ++p;
    }
    if (lx == 0) return;
    int i;
    do {
        p = path + l - 1;
        for (i = 0; i < lx; ++i) {
            *p = random_char();
            --p;
        }
    } while (file_exists(path));
}

FILE *create_open_tmp_file(const char *dir, const char *name, char *buf, RETURN_CODE *rc) {
    /* 一時ファイルを作成 */
    strcpy(buf, dir);
    strcat(buf, name);

    mkstemp2(buf); //bufは書き換えられる
    FILE *fp;
    if((fp = fopen(buf, "wb")) == NULL) {
        fprintf(stderr, "%s: err can not open(wb) file '%s'.\n", buf, opt_X);
        *rc = FILE_ERR;
        return NULL;
    }
    return fp;

/*    int fd = mkstemp(buf); //bufは書き換えられる
    if (fd == -1) {
        fprintf(stderr, "err can not create tmp file '%s'.\n", buf);
        *rc = FILE_ERR;
        return NULL;
    }*/
    /* バイナリ書き込みモードでファイルをオープン */
/*    FILE *fp;
    if((fp = fdopen(fd, "wb")) == NULL) {
        fprintf(stderr, "err can not open(wb) file '%s'.\n", buf);
        *rc = FILE_ERR;
        return NULL;
    }
    return fp;*/
}

bool process_image(MYMEMORY *buf, bool debug, RETURN_CODE *rc) {
    static time_t timestamp = 0;
    static char *timestamp_chars = NULL;

    //バッファからファイルポインタを取得
    FILE* memFp;
    if ((memFp = memory_to_readable_fp(buf)) == NULL) {
        fprintf(stderr, "%s: err can not get readable file pointer from memory.\n", opt_X);
        *rc = MEMORY_ERR;
        return false;
    }

    //jpegデータからimage_tを作成
    //image_t *pImg = read_jpeg_stream(memFp);
    image_t *pImg = read_jpeg_streamEx(memFp, opt_c); //オプションに応じて回転して読み込む
    fclose(memFp);

    if (pImg == NULL) {
        fprintf(stderr, "%s: err can not convert jpeg to image_t on memory.\n", opt_X);
        *rc = LIBJPEG_ERR;
        return false;
    }

    if (debug)
        fprintf(stderr, "%s: image size: %d x %d\n", opt_X, pImg->width, pImg->height);

    //テキストを描画
    if (opt_X[0] != '\0' && opt_x != NOT_DRAW) {
        draw_text(pImg, opt_X, opt_x);
    }

    //タイムスタンプを描画
    if (opt_t != NOT_DRAW) {
        time_t now = time(NULL);
        if (now != timestamp) {
            timestamp = now;
            timestamp_chars = generate_timestamp_chars(timestamp);
        }
        draw_text(pImg, timestamp_chars, opt_t);
    }

    bool retry = false;
    result_t res;
redo:
    //バッファに書き込むためのファイルポインタを取得
    if ((memFp = memory_to_writable_fp(buf)) == NULL) {
        fprintf(stderr, "%s: err can not get writable file pointer from memory.\n", opt_X);
        free_image(pImg);
        *rc = MEMORY_ERR;
        return false;
    }
    //image_tからjpegデータを作成
    res = write_jpeg_stream(memFp, pImg, opt_q);
    if (res != SUCCESS && !retry) {
        fclose(memFp);

        //失敗した場合は、１度だけバッファサイズを２倍にしてリトライ
        if (!alloc_memory(buf, buf->allocated_size * 2, REALLOC_GROW_ONLY, false)) {
            fprintf(stderr, "%s: err can not grow %d bytes of memory.\n",
                 opt_X, buf->allocated_size * 2);
            free_image(pImg);
            *rc = MEMORY_ERR;
            return false;
        }
        retry = true;
        goto redo;
    }

    free_image(pImg);
                
    if (res != SUCCESS) {
        fprintf(stderr, "%s: err can not convert image_t to jpeg on memory.\n", opt_X);
        fclose(memFp);
        *rc = LIBJPEG_ERR;
        return false;
    }

    //メモリに書き込まれたサイズを取得する
    buf->active_size = ftell(memFp);
    fclose(memFp);
    return true;
}

char *generate_timestamp_chars(time_t now) {
    struct tm *ts = localtime(&now);
    int Y = ts->tm_year - 100;  //2桁
    int M = ts->tm_mon + 1;
    int D = ts->tm_mday;
    int H = ts->tm_hour;
    int I = ts->tm_min;
    int S = ts->tm_sec;

    static char buf[20]; //YY/MM/DD HH:MM:SS
    char *p = &buf[0];
    itoa(Y, p, 10); p += 2;
    *p++ = '/';
    if (M < 10) *p++ = '0';
    itoa(M, p, 10);
    p += (M < 10) ? 1 : 2;
    *p++ = '/';
    if (D < 10) *p++ = '0';
    itoa(D, p, 10);
    p += (D < 10) ? 1 : 2;
    *p++ = ' ';
    if (H < 10) *p++ = '0';
    itoa(H, p, 10);
    p += (H < 10) ? 1 : 2;
    *p++ = ':';
    if (I < 10) *p++ = '0';
    itoa(I, p, 10);
    p += (I < 10) ? 1 : 2;
    *p++ = ':';
    if (S < 10) *p++ = '0';
    itoa(S, p, 10);

    return &buf[0];
}

/*void set_pixel(image_t *img, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (img == NULL ||
        img->color_type != COLOR_TYPE_RGB ||
        x < 0 || x >= img->width ||
        y < 0 || y >= img->height) return;
    pixcel_t *p = &img->map[y][x];
    p->r = r;
    p->g = g;
    p->b = b;
}*/

void draw_text(image_t *img, char *text, DRAWTEXTPOSITION pos) {
    if (img == NULL ||
        img->color_type != COLOR_TYPE_RGB ||
        img->height < 8 ||
        text == NULL) return;

    int l = strlen(text);
    int w = l << 3;
    if (img->width < w) return;

    int x,y;
    switch (pos) {
    case LEFT_TOP:
        x = 0;
        y = 0;
        break;
    case RIGHT_TOP:
        x = img->width - w;
        y = 0;
        break;
    case RIGHT_BOTTOM:
        x = img->width - w;
        y = img->height - 8;
        break;
    case LEFT_BOTTOM:
        x = 0;
        y = img->height - 8;
        break;
    default:
        return;
    }
    if (x < 0 || y < 0) return;

    unsigned char *p;
    pixcel_t *pp;
    unsigned char b;
    while (*text != '\0') {
        p = get_88font(*text++);
        if (p != NULL) {
            for (int yy = 0; yy < 8; ++yy) {
                b = *p++;
                pp = &img->map[y + yy][x];
                for (int xx = 0; xx < 8; ++xx) {
                    if (b & 1) {
                        pp->c.r = 0xff;
                        pp->c.g = 0xff;
                        pp->c.b = 0xff;
                    } else {
                        pp->c.r = 0x00;
                        pp->c.g = 0x00;
                        pp->c.b = 0x00;
                    }
                    b >>= 1;
                    ++pp;
                }
            }
        }        
        x += 8;
    }
}




/**
 * C++ version 0.4 char* style "itoa":
 * Written by Lukás Chmela
 * Released under GPLv3.

    */
char* itoa(int value, char* result, int base) {
    // check that the base if valid
    if (base < 2 || base > 36) { *result = '\0'; return result; }

    char* ptr = result, *ptr1 = result, tmp_char;
    int tmp_value;

    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz" [35 + (tmp_value - value * base)];
    } while ( value );

    // Apply negative sign
    if (tmp_value < 0) *ptr++ = '-';
    *ptr-- = '\0';
    while(ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr--= *ptr1;
        *ptr1++ = tmp_char;
    }
    return result;
}

//-1:引数エラー 0:ヘルプを表示した 1:オプションOK。処理続行
int proc_args(int argc, char *argv[]) {

    /* ヘルプの表示 */
    if ((argc >=2 && strncmp(argv[1], "-h", 2) == 0) || argc < 4) {
        fprintf(stderr, "ネットワークカメラなどのモーションJPEG(.mjpg)をファイルに保存する\n");
        fprintf(stderr, "処理の停止はSIGINT(CTRL+C)で行えます\n");
        fprintf(stderr, "第１引数　mjpg出力ディレクトリ。/tmp/など（\"\"で囲える）\n");
        fprintf(stderr, "第２引数　mjpg出力ファイル名（ディレクトリ、拡張子なし、\"\"で囲える）。例 video-%%Y%%m%%d-%%H%%M%%Sなど\n");
        fprintf(stderr, "　　　　　次の書式文字列を使用できる\n");
        fprintf(stderr, "　　　　　%%Y %%M %%D %%H %%I %%S 開始時の 西暦4桁 月2桁 日2桁 時2桁 分2桁 秒2桁\n");
        fprintf(stderr, "　　　　　%%y %%m %%d %%h %%i %%s 終了時の 西暦4桁 月2桁 日2桁 時2桁 分2桁 秒2桁\n");
        fprintf(stderr, "　　　　　%%f 保存されたフレーム数\n");
        fprintf(stderr, "　　　　　%%t 保存された長さ（秒）\n");
        fprintf(stderr, "　　　　　%%n ファイル番号\n");
        fprintf(stderr, "第３引数　ホスト名。192.168.0.26など\n");
        fprintf(stderr, "第４引数以降はオプション。n:整数 s:文字列（\"\"で囲える） w:特定のワード\n");
        fprintf(stderr, "　　　　　-u s リクエスト先のURI（未指定の場合は /?action=stream ）\n");
        fprintf(stderr, "　　　　　-p n リクエスト先のポート番号（未指定の場合は 8080 ）\n");
        fprintf(stderr, "　　　　　-F n １ファイルに保存するフレーム枚数（-T,-Vオプションと併用不可）\n");
        fprintf(stderr, "　　　　　-T n １ファイルに保存する秒数（-F,-Vオプションと併用不可）\n");
        fprintf(stderr, "　　　　　-V n ファイルに書き出すタイミングを時刻の分に紐づける（-F,-Tオプションと併用不可）\n");
        fprintf(stderr, "　　　　　   n=1～60（但しnは60を割り切れる値であること）\n");
        fprintf(stderr, "　　　　　   例 n=10　任意の時刻の0,10,20,30,40,50分にファイルを吐き出す\n");
        fprintf(stderr, "　　　　　   例 n=30　任意の時刻の0,30分にファイルを吐き出す\n");
        fprintf(stderr, "　　　　　-N n 保存するファイル数（未指定の場合は無制限）\n");
        fprintf(stderr, "　　　　　-S n 保存するフレームを間引く（デフォルトは 1。-Rオプションと併用できない）\n");
        fprintf(stderr, "　　　　　   例 n=3　3フレームに1回だけ保存する。フレームレートはソースの1/3になる\n");
        fprintf(stderr, "　　　　　-R n 保存するフレームレートを設定する（-Sオプションと併用できない）\n");
        fprintf(stderr, "　　　　　   ソースのフレームレートとの差分はフレームの複製または破棄によって調整される\n");
        fprintf(stderr, "　　　　　-I n 開始ファイル番号を指定。%%n書式で使用する（未指定の場合は 1 ）\n");
        fprintf(stderr, "　　　　　-t w タイムスタンプの表示位置 LT:左上 RT:右上 RB:右下 LB:左下（未指定は表示しない）\n");
        fprintf(stderr, "　　　　　-X s 任意のテキストを表示（日本語不可）\n");
        fprintf(stderr, "　　　　　-x w -Xオプションのテキスト表示位置 LT:左上 RT:右上 RB:右下 LB:左下（未指定はLT）\n");
        fprintf(stderr, "　　　　　-c n 時計回りに指定角度回転して保存する。 n=0,90,180,270 のいずれか\n");
        fprintf(stderr, "　　　　　-q n -tまたは-X,-cオプション使用時のjpeg出力品質[%%]（未指定は75）\n");
        fprintf(stderr, "　　　　　-m s サムネイルの出力ディレクトリを指定する。ファイル名はmjpgと同じで拡張子はjpgになる。例 /tmp/thumb/\n");
        fprintf(stderr, "　　　　　-i n 何も出力せずに入力ビデオの情報のみを出力する特殊オプション。nは調査する秒数（1以上。0または未指定で無効）\n");
        return 0;
    }

    if (!copy_dir(output_dir, sizeof(output_dir), argv[1])) {
        fprintf(stderr, "第1引数エラー。長すぎです\n");
        return -1;
    }

    if (!copy_dir(output_name, sizeof(output_name), argv[2])) {
        fprintf(stderr, "第2引数エラー。長すぎです\n");
        return -1;
    }

    if (strlen(argv[3]) >= sizeof(target_host)) {
        fprintf(stderr, "第3引数エラー。長すぎです\n");
        return -1;
    }
    strcpy(target_host, argv[3]);

    if (argc > 4 && (argc & 1) == 1) {
        fprintf(stderr, "オプションのパラメータ数が合致しません\n");
        return -1;
    }

    int i = 4;
    bool b = true;
    char c, *p;
    while (argc > i) {
        if (b) {
            if (argv[i][0] != '-') {
                fprintf(stderr, "第%d引数エラー。オプションは-で始めます\n", i);
                return -1;
            }
            if (strlen(argv[i]) != 2) {
                fprintf(stderr, "第%d引数エラー。オプションは2文字です\n", i);
                return -1;
            }
            c = argv[i][1];
            switch (c) {
                case 'F':
                case 'T':
                case 'V':
                case 'N':
                case 'I':
                case 'u':
                case 'p':
                case 't':
                case 'X':
                case 'x':
                case 'q':
                case 'm':
                case 'S':
                case 'R':
                case 'i':
                case 'c':
                    break;
                default:
                    fprintf(stderr, "第%d引数エラー。不明なオプション\n", i);
                    return -1;
            }
            b = false;
        } else {
            switch (c) {
                case 'F':
                    opt_F = atoi(argv[i]);
                    if (opt_F <= 0) {
                        fprintf(stderr, "第%d引数エラー。-Fオプションは1以上です\n", i);
                        return -1;
                    }
                    if (opt_T != -1 || opt_V != -1) {
                        fprintf(stderr, "第%d引数エラー。-F -T -V オプションは併用できません\n", i);
                        return -1;
                    }
                    break;
                case 'T':
                    opt_T = atoi(argv[i]);
                    if (opt_T <= 0) {
                        fprintf(stderr, "第%d引数エラー。-Tオプションは1以上です\n", i);
                        return -1;
                    }
                    if (opt_F != -1 || opt_V != -1) {
                        fprintf(stderr, "第%d引数エラー。-F -T -V オプションは併用できません\n", i);
                        return -1;
                    }
                    break;
                case 'V':
                    opt_V = atoi(argv[i]);
                    if (opt_V < 1 || opt_V > 60) {
                        fprintf(stderr, "第%d引数エラー。-Vオプションは1以上60以下です\n", i);
                        return -1;
                    }
                    if (60 % opt_V != 0) {
                        fprintf(stderr, "第%d引数エラー。-Vオプションは60を割り切れる値のみです\n", i);
                        return -1;
                    }
                    if (opt_F != -1 || opt_T != -1) {
                        fprintf(stderr, "第%d引数エラー。-F -T -V オプションは併用できません\n", i);
                        return -1;
                    }
                    break;
                case 'N':
                    opt_N = atoi(argv[i]);
                    if (opt_N == 0) {
                        fprintf(stderr, "第%d引数エラー。-Nオプションは1以上です\n", i);
                        return -1;
                    }
                    break;
                case 'S':
                    if (opt_R != -1) {
                        fprintf(stderr, "第%d引数エラー。-S -R オプションは併用できません\n", i);
                        return -1;
                    }
                    opt_S = atoi(argv[i]);
                    if (opt_S <= 0 || opt_S > 60) {
                        fprintf(stderr, "第%d引数エラー。-Sオプションは1以上、60以下です\n", i);
                        return -1;
                    }
                    break;
                case 'R':
                    if (opt_S != -1) {
                        fprintf(stderr, "第%d引数エラー。-S -R オプションは併用できません\n", i);
                        return -1;
                    }
                    opt_R = atoi(argv[i]);
                    if (opt_R < 1 || opt_R > 30) {
                        fprintf(stderr, "第%d引数エラー。-Rオプションは1以上、30以下です\n", i);
                        return -1;
                    }
                    break;
                case 'I':
                    opt_I = atoi(argv[i]);
                    break;
                case 'u':
                    if (!copy_dir(opt_u, sizeof(opt_u) - 1, argv[i])) {
                        fprintf(stderr, "第%d引数エラー。-uオプションが長すぎ\n", i);
                        return -1;
                    }
                    if (opt_u[strlen(opt_u) - 1] != '/')
                        strcat(opt_u, "/");
                    break;
                case 'p':
                    opt_p = atoi(argv[i]);
                    if (opt_p < 1 || opt_p > 65535) {
                        fprintf(stderr, "第%d引数エラー。-pオプションのポート番号が範囲外\n", i);
                        return -1;
                    }
                    break;
                case 't':
                    if (strcmp(argv[i],"LT") == 0)
                        opt_t = LEFT_TOP;
                    else if (strcmp(argv[i],"RT") == 0)
                        opt_t = RIGHT_TOP;
                    else if (strcmp(argv[i],"RB") == 0)
                        opt_t = RIGHT_BOTTOM;
                    else if (strcmp(argv[i],"LB") == 0)
                        opt_t = LEFT_BOTTOM;
                    else {
                        fprintf(stderr, "第%d引数エラー。-tオプションのワードが不適切\n", i);
                        return -1;
                    }
                    break;
                case 'X':
                    if (strlen(argv[i]) >= sizeof(opt_X)) {
                        fprintf(stderr, "第%d引数エラー。-Xオプションの文字列が長すぎ\n", i);
                        return -1;
                    }
                    p = argv[i];
                    strcpy(opt_X, (*p == '"') ? p + 1 : p);
                    if (*p == '"') {
                        p = strchr(opt_X, '"');
                        if (p != NULL) *p = '\0';
                    }
                    break;
                case 'x':
                    if (strcmp(argv[i],"LT") == 0)
                        opt_x = LEFT_TOP;
                    else if (strcmp(argv[i],"RT") == 0)
                        opt_x = RIGHT_TOP;
                    else if (strcmp(argv[i],"RB") == 0)
                        opt_x = RIGHT_BOTTOM;
                    else if (strcmp(argv[i],"LB") == 0)
                        opt_x = LEFT_BOTTOM;
                    else {
                        fprintf(stderr, "第%d引数エラー。-xオプションのワードが不適切\n", i);
                        return -1;
                    }
                    break;
                case 'q':
                    opt_q = atoi(argv[i]);
                    if (opt_q < 1 || opt_q > 100) {
                        fprintf(stderr, "第%d引数エラー。-qオプションは1～100[%%]です\n", i);
                        return -1;
                    }
                    break;
                case 'm':
                    if (strlen(argv[i]) >= sizeof(opt_m)) {
                        fprintf(stderr, "第%d引数エラー。-mオプションのパスが長すぎ\n", i);
                        return -1;
                    }
                    p = argv[i];
                    strcpy(opt_m, (*p == '"') ? p + 1 : p);
                    if (*p == '"') {
                        p = strchr(opt_X, '"');
                        if (p != NULL) *p = '\0';
                    }
                    break;
                case 'i':
                    opt_i = atoi(argv[i]);
                    if (opt_i < 0 || opt_i > 60) {
                        fprintf(stderr, "第%d引数エラー。-iオプションは0以上、60以下です\n", i);
                        return -1;
                    }
                    break;
                case 'c':
                    switch (atoi(argv[i])) {
                        case 0:   opt_c = ROTATE_CLOCKWISE_0; break;
                        case 90:  opt_c = ROTATE_CLOCKWISE_90; break;
                        case 180: opt_c = ROTATE_CLOCKWISE_180; break;
                        case 270: opt_c = ROTATE_CLOCKWISE_270; break;
                        default:
                            fprintf(stderr, "第%d引数エラー。-cオプションは0,90,180,270のみ有効です\n", i);
                            return -1;
                    }
                    break;
            }
            b = true;
        }
        ++i;
    }
    return 1;
}

/* 文字列の末尾のポインタを返す */
char *move_to_end(char *p) {
    while (*p != '\0') ++p;
    return p;
}

/* output_nameからファイル名を生成 */
void create_output_filename(char *dst, time_t tm_start, time_t tm_end, int read_frame_count, int file_number) {
    struct tm *ts;

    ts = localtime(&tm_start);
    int Y = ts->tm_year + 1900;
    int M = ts->tm_mon + 1;
    int D = ts->tm_mday;
    int H = ts->tm_hour;
    int I = ts->tm_min;
    int S = ts->tm_sec;

    ts = localtime(&tm_end);
    int y = ts->tm_year + 1900;
    int m = ts->tm_mon + 1;
    int d = ts->tm_mday;
    int h = ts->tm_hour;
    int i = ts->tm_min;
    int s = ts->tm_sec;

    char *p = &output_name[0];
    bool f = false;
    while (*p != '\0') {
        if (f) {
            switch (*p) {
                case 'Y':
                    itoa(Y, dst, 10); dst = move_to_end(dst); break;
                case 'M':
                    if (M < 10) *dst++ = '0';
                    itoa(M, dst, 10); dst = move_to_end(dst); break;
                case 'D':
                    if (D < 10) *dst++ = '0';
                    itoa(D, dst, 10); dst = move_to_end(dst); break;
                case 'H':
                    if (H < 10) *dst++ = '0';
                    itoa(H, dst, 10); dst = move_to_end(dst); break;
                case 'I':
                    if (I < 10) *dst++ = '0';
                    itoa(I, dst, 10); dst = move_to_end(dst); break;
                case 'S':
                    if (S < 10) *dst++ = '0';
                    itoa(S, dst, 10); dst = move_to_end(dst); break;
                case 'y':
                    itoa(y, dst, 10); dst = move_to_end(dst); break;
                case 'm':
                    if (m < 10) *dst++ = '0';
                    itoa(m, dst, 10); dst = move_to_end(dst); break;
                case 'd':
                    if (d < 10) *dst++ = '0';
                    itoa(d, dst, 10); dst = move_to_end(dst); break;
                case 'h':
                    if (h < 10) *dst++ = '0';
                    itoa(h, dst, 10); dst = move_to_end(dst); break;
                case 'i':
                    if (i < 10) *dst++ = '0';
                    itoa(i, dst, 10); dst = move_to_end(dst); break;
                case 's':
                    if (s < 10) *dst++ = '0';
                    itoa(s, dst, 10); dst = move_to_end(dst); break;
                case 'f':
                    itoa(read_frame_count, dst, 10); dst = move_to_end(dst); break;
                case 't':
                    itoa(tm_end - tm_start, dst, 10);
                    dst = move_to_end(dst); break;
                case 'n':
                    itoa(file_number, dst, 10); dst = move_to_end(dst); break;
                default:
                    *dst++ = '%';
                    *dst++ = *p;
            }
            f = false;
        } else if (*p == '%') {
            f = true;
        } else {
            *dst++ = *p;
        }
        ++p;
    }
    *dst = '\0';
}

//コマンドライン引数のダブルクォーテーション除去
bool copy_dir(char *dst, int buflen, char *src) {
    bool b = (*src == '"');
    if (b) ++src;
    char *p = src;
    int n = 0;
    while (*p != '\0') {
        if (b && *p == '"') break;
        ++n;
        ++p;
    }
    if (n >= buflen) return false;
    memcpy(dst, src, n);
    *(dst + n) = '\0';
    return true;
}

//ファイルコピー
/*int fcopy(const char *fnamer, const char *fnamew) {
	FILE *fpr = fopen(fnamer, "rb");
	FILE *fpw = fopen(fnamew, "wb");
	if (fpr == NULL || fpw == NULL) {
        if (fpr != NULL) fclose(fpr); 
        if (fpw != NULL) fclose(fpw); 
		return -1;
	}

    char buf[1024];
    while (1) {
        size_t sz = fread(buf, 1, sizeof(buf), fpr);
        if (sz == 0) break;
        fwrite(buf, 1, sz, fpw);
    }

	fclose(fpw);
	fclose(fpr);
    return 0;
}*/

#define HTTP_BUF_SIZE 1024
static char http_res[HTTP_BUF_SIZE];
static int pop_char = -1;
static int http_res_len = 0;
static int http_res_read_index = 0;

int read_char() {
    if (pop_char != -1) {
        int c = pop_char;
        pop_char = -1;
        return c;
    }
    if (http_res_len == http_res_read_index) {
        http_res_len = recv(sofd, http_res, sizeof(http_res), 0);
        http_res_read_index = 0;
    }
    if (http_res_len == http_res_read_index)
        return -1;
    return http_res[http_res_read_index++] & 255;
}

void push_char(char c) {
    pop_char = (unsigned int)((unsigned char)c);
}

bool skip_until(const char *word, int limit) {
    int c, l = limit;
    const char *p = word;
    while (1) {
        c = read_char();
        if (c == -1) return false;
        if (c == *p) {
            if (*++p == '\0') return true;
        } else {
            p = word;
        }
        if (limit > 0 && --l <= 0) return false;
    }
}

//\r\n\r\n出現までスキップ
//Content-Length値を返す
//エラーの場合は-1を返す
int skip_until_content_start() {
    const char *content_length_word = "Content-Length:";
    int content_length = -1;
    const char *p = content_length_word;
    bool buffering = false;
    char buf1[16];
    int j = 0;
    char buf2[5];
    buf2[4] = '\0';
    int i = 0;
    int c;
    int limit = 1000;
    while (1) {
        c = read_char();
        if (c == -1) return -1; //no more data

        if (content_length == -1) {
            if (!buffering) {
                if (c == *p) {
                    if (*++p == '\0') buffering = true;
                } else {
                    p = content_length_word;
                }
            } else {
                if (c != '\r' && c != '\n') {
                    buf1[j++] = c;
                    if (j == sizeof(buf1)) return -1; //buf1[] full
                } else {
                    buf1[j] = '\0';
                    content_length = atoi(buf1);
                }
            }
        }

        buf2[i] = c;
        i = ++i & 3;
        if (c == '\n' && (strcmp(buf2, "\r\n\r\n") == 0 || strcmp(buf2, "\n\r\n\r") == 0))
            return (content_length == -1) ? 0 : content_length;

        if (--limit == 0) return -1;
    }
}

bool read_to_mem(MYMEMORY *pMem, int size) {
    char *p = (char *)pMem->ptr;
    int n = size;
    while (n-- > 0) {
        int c = read_char();
        if (c == -1) return false;
        *p++ = c;
    }
    pMem->active_size = size;
    return true;
}

bool read_to_throwaway(int size) {
    int n = size;
    while (n-- > 0) {
        if (read_char() == -1) return false;
    }
    return true;
}

//border_minuteは1～60の値かつ60を割り切れる値であること
time_t get_next_boder_time(int border_minute) {
    time_t now = time(NULL);
    struct tm *pts = localtime(&now);
    int min = (int)(pts->tm_min / border_minute) * border_minute;
    while (min <= pts->tm_min) min += border_minute;
    struct tm ts;
    memcpy(&ts, pts, sizeof(ts));
    ts.tm_min = min;
    ts.tm_sec = 0;
    return mktime(&ts);
}

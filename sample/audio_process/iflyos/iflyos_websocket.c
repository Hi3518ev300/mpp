#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

#include <uwsc/uwsc.h>

#include "utils.h"
#include "iflyos_defines.h"

static BOOL g_sampling = TRUE;
static BOOL g_playing = TRUE;
static int g_stop_capture = 0;



#define PRINT(fmt, ...) printf("%s   "fmt, get_cur_time(), ##__VA_ARGS__)
#define ERROR(fmt, ...) printf("%s   "fmt" :%s\n", get_cur_time(), ##__VA_ARGS__, strerror(errno))

char *get_cur_time()
{
    static char s[20] = {0};
    time_t t;
    struct tm* ltime;
    
    time(&t);
    ltime = localtime(&t);
    strftime(s, 20, "%Y-%m-%d %H:%M:%S", ltime);
    s[19] = '\0';

    return s;
}

static void thread_send_pcm_cb(void *data)
{
    if (NULL == data)
    {
        return;
    }    

    struct uwsc_client *cl = (struct uwsc_client *)data;
    int read_count = 0; 
    char pcm_buf[640] = {0};

    FILE *fp  = fopen("/tmp/test.pcm", "w");
    if(fp == NULL)
    {
        utils_print("create or open test.pcm error\n");
        return;
    }

    int fd = iflyos_get_audio_data_handle();
    if(-1 == fd)
    {
        return;
    }
    while(g_sampling)
    {
        read_count = read(fd, pcm_buf, 1024);
        if (read_count >0)
        {   
            //send request 
            char *req = iflyos_create_audio_in_request();
            utils_print("To send request....\n");
            cl->send(cl, req, strlen(req), UWSC_OP_TEXT);
            free(req);
            //send data 
            // usleep(100);
            // cl->send(cl, pcm_buf, 2048, UWSC_OP_BINARY);
            // utils_print("To send pcm bin data....\n");
            //write into test file
            fwrite(pcm_buf, sizeof(char), 1024, fp);
            fflush(fp);
            //

            if (g_stop_capture)
            {
                utils_print("To send END flag !!!!!\n");

                cl->send(cl, "__END__", strlen("__END__"), UWSC_OP_BINARY);
                g_stop_capture = 0;
            }
        }
        usleep(200 * 1000);
    }
    utils_print("sample thread exit...\n");
    fclose(fp);
    close(fd);
}

static void thread_play_mp3_cb(void *data)
{
    board_play_mp3_fifo(g_playing);
}

static void uwsc_onopen(struct uwsc_client *cl)
{
    uwsc_log_info("iflyos onopen\n");

    // added by hekai
    pthread_t tid1;
    pthread_create(&tid1, NULL, thread_send_pcm_cb, (void*)cl);
    pthread_detach(tid1);

    // pthread_t tid2;
    // pthread_create(&tid2, NULL, thread_play_mp3_cb, NULL);
    // pthread_detach(tid2);
    // end added
}

static void uwsc_onmessage(struct uwsc_client *cl,
	void *data, size_t len, bool binary)
{
    printf("iflyos recv:\n");

    if (binary) {
        //文件
    } 
    else 
    {
        PRINT("[%.*s]\n", (int)len, (char *)data);
        char* name = iflyos_get_response_name(data);
        if (NULL == name)
        {
            return;
        }
        if(strcmp(name, aplayer_audio_out) == 0)
        {
            //iflyos_play_response_audio(data);
        }
        else if (strcmp(name, recog_stop_capture) == 0)
        {
            g_stop_capture = 1;
        }
        free(name);
    }
    //printf("Please input:\n");
}

static void uwsc_onerror(struct uwsc_client *cl, int err, const char *msg)
{
    utils_print("iflyos onerror:%d: %s\n", err, msg);
    ev_break(cl->loop, EVBREAK_ALL);
}

static void uwsc_onclose(struct uwsc_client *cl, int code, const char *reason)
{
    utils_print("iflyos onclose:%d: %s\n", code, reason);
    //added by hekai
    iflyos_deinit_request();
    g_sampling = FALSE;
    //end added
    
    ev_break(cl->loop, EVBREAK_ALL);
}

static void signal_cb(struct ev_loop *loop, ev_signal *w, int revents)
{
    if (w->signum == SIGINT) {

        g_sampling = FALSE;
        g_playing = FALSE;
        ev_break(loop, EVBREAK_ALL);
        utils_print("Normal quit\n");
    }
}

int iflyos_websocket_start()
{
    struct ev_loop *loop = EV_DEFAULT;
    struct ev_signal signal_watcher;
	int ping_interval = 10;	/* second */
    struct uwsc_client *cl;

    iflyos_load_cfg();

    char ifly_url[255] = {0};
    char* device_id = iflyos_get_device_id();
    char* token = iflyos_get_token();
    sprintf(ifly_url, "wss://ivs.iflyos.cn/embedded/v1?token=%s&device_id=%s", token, device_id); 

    
    cl = uwsc_new(loop, ifly_url, ping_interval, NULL);
    if (!cl)
        return -1;

	utils_print("iflyos connect...\n");

    cl->onopen = uwsc_onopen;
    cl->onmessage = uwsc_onmessage;
    cl->onerror = uwsc_onerror;
    cl->onclose = uwsc_onclose;
    
    ev_signal_init(&signal_watcher, signal_cb, SIGINT);
    ev_signal_start(loop, &signal_watcher);

    ev_run(loop, 0);
    free(cl);       
    iflyos_unload_cfg();

    return 0;
}
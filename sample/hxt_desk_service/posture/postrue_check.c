#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <errno.h>

#include <sitting_posture.h>

#include "utils.h"
#include "common.h"
#include "server_comm.h"
#include "hxt_defines.h"
#include "report_info_db.h"
#include "databuffer.h"

#define NORMAL_POSTURE_STATUS         0
#define BAD_POSTURE_STATUS            1
#define AWAY_STATUS                   2

#define FIRST_DEPART_ALARM_TIMEVAL          (3*60)
#define LAST_DEPART_ALARM_TIMEVAL           (5*60)         
#define ALARM_TIMEVAL                       (3*60)          
#define BAD_ALARM_TIMEVAL                   (10)
#define CORRECT_JUDGE_TIMEVAL               (5)             
#define MIN_DURATION_TIME                   (3)

/*
* width: 识别图像宽度
* height: 识别图像高度
* alarm_interval: 持续错误坐姿告警时间
* video_duration: 错误坐姿视频录制时间
* sanp_freq:抽帧频次
*/
typedef struct posture_param_s
{
    int width;
    int height;
    int alarm_interval;
    int video_duration;
    int snap_freq;
    int study_mode;
}PostureParams;

typedef struct check_status_t
{
    time_t _start_time;
    time_t _last_time;
    time_t _last_alarm_time;
    time_t _last_record_time;
    int _start_posture;
    int _last_posture;
};

extern DATABUFFER g_msg_buffer;
extern BOOL g_hxt_wbsc_running;

static void* g_recog_handle = NULL;
static pthread_t g_proc_yuv_tid = NULL;
static BOOL g_keep_processing = FALSE;
static BOOL g_is_recording = FALSE;
static BOOL g_is_inited = FALSE;
static BOOL g_first_alarm = TRUE;
static BOOL g_first_away = TRUE;
static char g_mp4_file[128] = {0};
static char g_snap_file[128] = {0};
// static int g_msg_qid;

static void play_random_warn_voice()
{
    char* voice[5] = {VOICE_SITTING_WARM1, VOICE_SITTING_WARM2, VOICE_SITTING_WARM3, VOICE_SITTING_WARM4, VOICE_SITTING_WARM5};  
    char self_def_voice[128] = {0};  
    int chlid_unid = hxt_get_child_unid();
    int alarm_type = hxt_get_alarm_type_cfg(chlid_unid);
    int idx = rand() % 5;
    switch(alarm_type)
    {
        case 0:
            /* mute */
        break;
        case 1:
            utils_send_local_voice(VOICE_WARNING_BUZZ);
        break;
        case 2:
            utils_send_local_voice(voice[idx]);
        break;
        case 3:
            bzero(self_def_voice, 128);
            sprintf(self_def_voice, HXT_CHILD_ALARM_FILE, chlid_unid, idx);
            utils_send_local_voice(self_def_voice);
        default:
            utils_send_local_voice(voice[idx]);
        break;
    }
}

static void play_random_praise_voice()
{
    char* voice[5] = {VOICE_SITTING_PRAISE1, VOICE_SITTING_PRAISE2, VOICE_SITTING_PRAISE3, VOICE_SITTING_PRAISE4, VOICE_SITTING_PRAISE5};
    int idx = rand() % 5;
    
    utils_send_local_voice(voice[idx]);
}

static void take_rest(int time_ms)
{
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = time_ms * 1000;

    select(0, NULL, NULL, NULL, &timeout);
}

/* cmd with mpp service, to control record or snap */
static BOOL begin_recording()
{
    /* if no child bind, no need record */
    int child_unid = hxt_get_child_unid();
    if (child_unid == -1)
    {
        return FALSE;
    }

    if(!g_is_recording)
    {
        time_t now = time(0);
        struct tm *_tm = localtime(&now);
        
        bzero(g_mp4_file, 128);
        bzero(g_snap_file, 128);
        snprintf(g_mp4_file, 128, "/user/child_%d/video/%04d%02d%02d-%02d%02d%02d.mp4",
                            child_unid,
                            _tm->tm_year + 1900,
                            _tm->tm_mon + 1, 
                            _tm->tm_mday,
                            _tm->tm_hour,
                            _tm->tm_min,
                            _tm->tm_sec);

        snprintf(g_snap_file, 128, "/user/child_%d/snap/%04d%02d%02d-%02d%02d%02d.jpg",
                                child_unid,
                                _tm->tm_year + 1900,
                                _tm->tm_mon + 1, 
                                _tm->tm_mday,
                                _tm->tm_hour,
                                _tm->tm_min,
                                _tm->tm_sec);
        send_recording_mp4_cmd(g_mp4_file, g_snap_file);
        g_is_recording = TRUE;
        utils_print("%s -----> Begin record\n", utils_get_current_time());
    }
    
    return TRUE;
}

static void delete_recorded()
{
    if(g_is_recording)
    {
        send_delete_mp4_cmd();
        g_is_recording = FALSE;
        utils_print("%s -----> Delete record: %s\n", utils_get_current_time(), g_mp4_file);
    }
}

static void stop_record()
{
    if(g_is_recording)
    {
        g_is_recording = FALSE;
        utils_print("%s -----> Stop record\n", utils_get_current_time());
        send_stop_record_mp4_cmd();
    }
}

static BOOL send_study_report_type(StudyInfo *info)
{
    /* if no child bind, no need to send message */
    int child_unid = hxt_get_child_unid();
    if (child_unid == -1)
    {
        utils_print("no child binded\n");
        return FALSE;
    }

    if (g_hxt_wbsc_running)
    {
        /* online mode */
        // info->msg_type = 1;
        // if (msgsnd(g_msg_qid, (void*)info, sizeof(StudyInfo) - sizeof(long), 0) < 0)
        // {
        //     utils_print("send study info msg failed, %s\n", strerror(errno));
        //     return FALSE;
        // }
        
        char *ptr = get_free_buffer(&g_msg_buffer, sizeof(StudyInfo));
        if (NULL == ptr)
        {
            utils_print("Msg buffer is full\n");
            return FALSE;
        }
        info->msg_type = 1;
        memcpy(ptr, (void*)info, sizeof(StudyInfo));
        use_free_buffer(&g_msg_buffer, sizeof(StudyInfo));
    }
    else
    {
        /* offline mode */
        ReportInfo report_info;
        bzero(&report_info, sizeof(ReportInfo));
        report_info.camera_status = hxt_get_camera_status();
        report_info.parent_unid = hxt_get_parent_unid_cfg();
        report_info.child_unid = hxt_get_child_unid();
        report_info.report_type = info->info_type;
        strcpy(report_info.study_date, utils_date_to_string());
        strcpy(report_info.report_time, utils_time_to_string());
        report_info.study_mode = hxt_get_study_mode_cfg(report_info.child_unid);
        if (report_info.report_type == BAD_POSTURE)
        {
            report_info.duration = 10;
            strcpy(report_info.video_url, info->file);
            strcpy(report_info.snap_url, info->snap);
        }
        add_report_info((void *)&report_info);
    }
    
    return TRUE;
}

static BOOL init_check_status(struct check_status_t *check_status, int check_result)
{
    if(!g_is_inited)
    {
        check_status->_start_posture = check_status->_last_posture = check_result;
        check_status->_start_time = check_status->_last_time = time(NULL);
        check_status->_last_record_time = time(NULL);
        check_status->_last_alarm_time = time(NULL);

        g_is_inited = TRUE;

        return TRUE;
    }

    return FALSE;
}

/* confirm if change to normal posture */
static BOOL check_posture_changed(struct check_status_t *check_status, int check_result)
{
    time_t now = time(NULL); 
    
    if (check_status->_start_posture == check_result)
    {
        check_status->_last_posture = check_result;
        check_status->_last_time = now;

        // 连续的错误姿势
        if (check_status->_start_posture == BAD_POSTURE_STATUS)
        {
            if (!g_is_recording)
            {
                check_status->_last_record_time = now;
            }
            begin_recording();
        }
        else
        {
            delete_recorded(); 
        }

        return FALSE;
    }

    if (check_status->_last_posture != check_result)
    {
        check_status->_last_posture = check_result;
        check_status->_last_time = now;
    }
    else
    {
        int interval = now - check_status->_last_time;
        if (interval >= MIN_DURATION_TIME)
        {
            if(check_status->_last_posture != BAD_POSTURE_STATUS)
            {
                delete_recorded();
            }
            else
            { 
                /* means already change to bad posture */
                if (!g_is_recording)
                {
                    check_status->_last_record_time = now;
                }
                begin_recording(); 
                
            }
            
            if (check_status->_start_posture == BAD_POSTURE_STATUS)
            {
                if (check_result == NORMAL_POSTURE_STATUS)
                {
                     play_random_praise_voice();
                     g_first_alarm = TRUE;
                }
            }

            if (check_status->_start_posture == AWAY_STATUS)
            {
                if(!g_first_away)
                {
                    utils_send_local_voice(VOICE_CHILD_REAPPEAR);
                    g_first_away = TRUE;
                }

                StudyInfo info;
                memset(&info, 0, sizeof(StudyInfo));
                info.info_type = CHILD_BACK;
                send_study_report_type(&info);
            }

            check_status->_start_posture = check_result;
            check_status->_start_time = now;
            check_status->_last_posture = check_result;
            check_status->_last_time = now;

            return TRUE;
        }    
    }      

    return FALSE;
}

static BOOL check_posture_alarm(struct check_status_t *check_status, int check_result, int video_time_len, int check_interval)
{
    time_t now = time(NULL);
    int interval = now - check_status->_start_time;
    int record_interval = now - check_status->_last_record_time;
    int start_posture = check_status->_start_posture;
    if (start_posture != check_result)
    {
        return FALSE;
    }

    switch (check_result)
    {
    case NORMAL_POSTURE_STATUS:
        break;
    case BAD_POSTURE_STATUS:
        if (record_interval >= video_time_len)
        {
            stop_record();
            check_status->_last_record_time = now;
            StudyInfo info;
            memset(&info, 0, sizeof(StudyInfo));
            info.info_type = BAD_POSTURE;
            strcpy(info.file, g_mp4_file);
            strcpy(info.snap, g_snap_file);
            send_study_report_type(&info);   
        }
        if (interval >= check_interval)
        {
            if(g_first_alarm)
            {
                play_random_warn_voice();
                utils_print("BAD POSTURE ALARM !!!\n");
                check_status->_last_alarm_time = now;
                g_first_alarm = FALSE;
            }
            else
            {
                if ((now - check_status->_last_alarm_time) >= ALARM_TIMEVAL)
                {
                    play_random_warn_voice();
                    utils_print("BAD POSTURE ALARM2222 !!!\n");
                    check_status->_last_alarm_time = now;
                }
            }
            check_status->_start_posture = check_result;
            check_status->_start_time = now;
        } 
               
    break;
    case AWAY_STATUS:
        if(interval >= FIRST_DEPART_ALARM_TIMEVAL)
        {
            if (g_first_away)
            {
                utils_send_local_voice(VOICE_CHILD_AWAY);
                utils_print("AWAY ALARM !!!\n");
                check_status->_start_time = now;
                g_first_away = FALSE;

                /* send message to hxt server */
                StudyInfo info;
                memset(&info, 0, sizeof(StudyInfo));
                info.info_type = CHILD_AWAY;
                send_study_report_type(&info);
            }
            else
            {
                if (interval >= LAST_DEPART_ALARM_TIMEVAL)
                {
                    utils_send_local_voice(VOICE_CAMERA_SLEEP);
                    utils_print("AWAY ALARM22222 !!!\n");
                    /* exit recog thread */
                    g_keep_processing = FALSE;

                    /* send message to hxt server */
                    StudyInfo info;
                    memset(&info, 0, sizeof(StudyInfo));
                    info.info_type = CHILD_AWAY;
                    send_study_report_type(&info);
                }
            }
            delete_recorded(); 
        }
    break;    
    default:
        break;
    }

    return TRUE;
}

static void* thread_proc_yuv_data_cb(void *param)
{
    int one_check_result = 0;
    BOOL inited_status = FALSE;
    BOOL change_to_correct = FALSE;
    BOOL first_away_alarm = TRUE;
    BOOL first_bad_alarm = TRUE;
    BOOL recording = FALSE;

    if(NULL == param)
    {
        return NULL;
    }

    PostureParams *params = utils_malloc(sizeof(PostureParams));
    memcpy(params, param, sizeof(PostureParams));
    if (params->alarm_interval == 0)
    {
        params->alarm_interval = 10;
    }
    if(params->video_duration == 0)
    {
        params->video_duration = 10;
    }
    if (params->study_mode == 0)
    {
        params->study_mode = 3;
    }
    
    struct check_status_t check_status;
    memset(&check_status, 0, sizeof(struct check_status_t));

    // g_msg_qid = msgget(STUDY_INFO_MQ_KEY, 0666 | IPC_CREAT);
    // if(g_msg_qid < 0)
    // {
    //     utils_print("create message queue error\n");
    //     return -1;
    // }

    StudyInfo info;
    memset(&info, 0, sizeof(StudyInfo));
    info.info_type = STUDY_BEGIN;
    send_study_report_type(&info);

    unsigned char *yuv_buf = NULL;
    utils_print("To process yuv data from vpss ....\n");
    while (g_keep_processing)
    {
        /* get yuv data from vpss */
        board_get_yuv_from_vpss_chn(&yuv_buf);
        if(NULL == yuv_buf)
        {
            utils_print("no vpss data...\n");
            continue;
        }
        /* recog posture */
        /* 0 : Normal */
        /* 1 : bad posture */
        /* 2 : away */
        one_check_result = run_sit_posture(g_recog_handle, yuv_buf, params->width, params->height, 5);
        utils_print("%s -----> %d\n", utils_get_current_time(), one_check_result);
        if (init_check_status(&check_status, one_check_result))
        {
            continue;
        }

        if(check_posture_changed(&check_status, one_check_result))
        {
            utils_print("POSTURE CHANGED\n");
            continue;
        }
        check_posture_alarm(&check_status, one_check_result, params->video_duration, params->alarm_interval);

        if (yuv_buf != NULL)
        {
            utils_free(yuv_buf);
            yuv_buf = NULL;
        }

        // take_rest(100); 
    }
    /* prevent some fragmentary video*/
    delete_recorded();
    /**/
    if (g_recog_handle != NULL)
    {
        uninit_sit_posture(&g_recog_handle);
    }

    utils_free(params);

    /* send msg to notify ending */
    memset(&info, 0, sizeof(StudyInfo));
    info.info_type = STUDY_END;
    send_study_report_type(&info);
    utils_send_local_voice(VOICE_CAMERA_SLEEP);

    utils_print("rocognize thread exit...\n");

    return NULL;
}

void start_posture_recognize()
{
    int bind_status = hxt_get_desk_bind_status_cfg();
    if (bind_status != 1)
    {
        utils_print("Desk not binded\n");
        return;
    }

    /* get config */
    if (!hxt_get_desk_cfg_request())
    {
        utils_print("Get config from server failed\n");
        return;
    }


    if (g_keep_processing)
    {
        return;
    }
    g_keep_processing = TRUE;

    PostureParams params;
    memset(&params, 0, sizeof(PostureParams));
    params.video_duration = hxt_get_video_length_cfg();
    params.alarm_interval = hxt_get_posture_judge_cfg();         
    params.width = hxt_get_video_height_cfg();
    params.height = hxt_get_video_width_cfg();
    params.snap_freq = hxt_get_attach_ratio_cfg();
    params.study_mode = hxt_get_study_mode_cfg();
    
    /* get posture pattern file */
    int study_mode = hxt_get_study_mode_cfg();

    char *model_path1 = hxt_get_posture_detect_model_path_cfg(study_mode);
    char *model_path2 = hxt_get_posture_class_model_path_cfg(study_mode);
    if(NULL == model_path1 || NULL == model_path2)
    {
        return;
    }
    
    send_posture_start_cmd(params.height, params.width);

    utils_send_local_voice(VOICE_NORMAL_STATUS);

    // init 
    utils_print("To start recognize....\n");
    g_recog_handle = init_sit_posture(model_path1, model_path2);
    if (NULL == g_recog_handle)
    {
        utils_print("posture model init failed\n");
    }
    pthread_create(&g_proc_yuv_tid, NULL, thread_proc_yuv_data_cb, (void*)&params);
   
    return;
}

void stop_posture_recognize()
{
    if (g_keep_processing)
    {
        send_posture_stop_cmd();
        /* to tell mpp service stop video system */
        utils_print("To stop recognize....\n");
        g_keep_processing = FALSE;
    }
}
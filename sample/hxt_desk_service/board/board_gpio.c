#include <gpiod.h>
#include <hi_comm_aio.h>

#include "utils.h"

#include "common.h"

#define FLASH_TIMES         3
#define LED_SLEEP_TIME      (200*1000)
/*
** LED状态 **
-----           ---------           ---------          ------ 
                 GPIO0_1              gpio01            红色 
                 GPIO9_2              gpio74            绿色    
                 GPIO2_4              gpio20            蓝色

                 GPIO2_5              gpio21            蓝色 
                 GPIO2_6              gpio22            红色 
                 GPIO2_7              gpio23            绿色

                 GPIO0_5              gpio05            蓝色 
                 GPIO9_4              gpio76            红色                 
                 GPIO10_1             gpio81            绿色                  
*/

/*
** 按键 **
                                      gpio66            vol-
                                      gpio68            vol+
                                      gpio86            mute
                                      gpio47            reset
*/

static struct gpiod_chip *gpio0 = NULL;
static struct gpiod_chip *gpio2 = NULL;
static struct gpiod_chip *gpio5 = NULL;
static struct gpiod_chip *gpio8 = NULL;
static struct gpiod_chip *gpio9 = NULL;
static struct gpiod_chip *gpio10 = NULL;

static struct gpiod_line *btn_vol_inc = NULL;
static struct gpiod_line *btn_vol_dec = NULL;
static struct gpiod_line *btn_mute = NULL;
static struct gpiod_line *btn_reset = NULL;

static struct gpiod_line *led_camera_green = NULL;
static struct gpiod_line *led_camera_blue = NULL;
static struct gpiod_line *led_camera_red = NULL;
static struct gpiod_line *led_wifi_green = NULL;
static struct gpiod_line *led_wifi_blue = NULL;
static struct gpiod_line *led_wifi_red = NULL;
static struct gpiod_line *led_mute_green = NULL;
static struct gpiod_line *led_mute_blue = NULL;
static struct gpiod_line *led_mute_red = NULL;

static AUDIO_DEV ao_dev = 0;
static BOOL dec_vol_pressed = FALSE;
static BOOL inc_vol_pressed = FALSE;

// static BOOL posture_running = FALSE;
static BOOL system_booting = TRUE;
static BOOL net_connecting = FALSE;
static BOOL system_reset = FALSE;
static BOOL qrcode_scanning = FALSE;

static int gpio_set_value(struct gpiod_line *line, int value)
{
    int ret = 0;
    gpiod_line_set_value(line, value);

    return ret;
}

static int gpio_get_value(struct gpiod_line *line)
{
    return gpiod_line_get_value(line);
}

static void* check_reset_event(void *param)
{
    struct gpiod_line_event event;
    time_t start = 0, end = 0;
    while(1)
    {
        gpiod_line_event_wait(btn_reset, NULL);
        if (gpiod_line_event_read(btn_reset, &event) != 0)
        {
            continue;
        }

        if(event.event_type == GPIOD_LINE_EVENT_RISING_EDGE)
        {
            printf("reset btn pressed down\n");
            end = time(0);

        }
        if(event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE)
        {
            printf("reset btn pressed up\n");
            start = time(0);
            printf("inteval is %ld\n", end - start);
            if (end - start > 5)
            {
                board_start_reset_led_blinking();
                /* reset */
                utils_system_reset();
                utils_system_reboot();
            }
            else
            {
                board_led_all_off();
                sleep(2);
                /* reboot */
                utils_system_reboot();
            }
            
        }
    }

    return;
}

static void* check_inc_vol_event(void *param)
{
    struct gpiod_line_event event;
    time_t start = 0, end = 0;
    while(1)
     {
        gpiod_line_event_wait(btn_vol_inc, NULL);
        if (gpiod_line_event_read(btn_vol_inc, &event) != 0)
        {
            continue;
        }
        if(event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE)
        {
            printf("inc vol btn pressed down\n");
            start = time(0);
        }
        if(event.event_type == GPIOD_LINE_EVENT_RISING_EDGE)
        {
            printf("inc vol btn pressed up\n");
            end = time(0);
            printf("inteval is %ld\n", end - start);
            if (end - start > 5)
            {
                inc_vol_pressed = 1;
            }
        }
        else
        {
            int current_vol = 0;
            HI_MPI_AO_GetVolume(ao_dev, &current_vol);
            if(current_vol == 5)
            {
                /*already max*/
            }
            else
            {
                HI_MPI_AO_SetVolume(ao_dev, current_vol+14);
            }
        }
     }
    return NULL;
}

static void* check_dec_vol_event(void *param)
{
    struct gpiod_line_event event;
    time_t start = 0, end = 0;
    while(1)
     {
        gpiod_line_event_wait(btn_vol_dec, NULL);
        if (gpiod_line_event_read(btn_vol_dec, &event) != 0)
        {
            continue;
        }
        if(event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE)
        {
            printf("dec vol btn pressed down\n");
            start = time(0);

        }
        if(event.event_type == GPIOD_LINE_EVENT_RISING_EDGE)
        {
            printf("dec vol btn press up\n");
            end = time(0);
            printf("inteval is %ld\n", end - start);
            if (end - start > 5)
            {
                dec_vol_pressed = TRUE;
            }
            else
            {
                int current_vol = 0;
                HI_MPI_AO_GetVolume(ao_dev, &current_vol);
                if(current_vol == -112)
                {
                    HI_MPI_AO_SetMute(ao_dev, HI_TRUE, NULL);
                }
                else
                {
                    HI_MPI_AO_SetVolume(ao_dev, current_vol-14);
                }
            }
        }
     }
    return NULL;
}

static void* check_posture_event(void *param)
{
    struct gpiod_line_event event;
    time_t start = 0, end = 0;
    BOOL posture_running = FALSE;

    while(1)
    {
        gpiod_line_event_wait(btn_mute, NULL);
        if (gpiod_line_event_read(btn_mute, &event) != 0)
        {
            continue;
        }
        
        if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE)
        {
            printf("camera btn pressed down\n");
            start = time(0);
        }
        if(event.event_type == GPIOD_LINE_EVENT_RISING_EDGE)
        {
            printf("camera btn pressed up\n");
            end = time(0);   
            printf("inteval is %ld\n", end - start);

            if (end - start >= 3)
            {
                /* standby */
                board_led_all_off();
                
            }
            else
            {
                if (!posture_running)
                {
                    posture_running = TRUE;
                    start_posture_recognize();   
                }
                else
                {
                    stop_posture_recognize();   
                    posture_running = FALSE;  
                }
            }
        }
    }
    return NULL;
}

static void* check_scan_qrcode_event(void *param)
{
    BOOL first_notice = TRUE;
    while(1)
    {
        if (inc_vol_pressed && dec_vol_pressed)
        {
            qrcode_scanning = TRUE;

            if(first_notice)
            {
                utils_send_local_voice(VOICE_SCAN_QRCODE);
                first_notice = FALSE;
            }

            board_start_connect_led_blinking();

            printf("To scan qrcode....\n");
            inc_vol_pressed = dec_vol_pressed = FALSE;
            while (!get_qrcode_yuv_buffer())
            {
                utils_send_local_voice(VOICE_WIFI_BIND_ERROR);
                sleep(3);
                continue;
            }
            break;
        }
        sleep(1);
    }

    qrcode_scanning = FALSE;

    return NULL;
}

static void* led_blinking_thread(void* param)
{
    while(1)
    {
        if(system_booting)
        {
            gpio_set_value(led_camera_blue, 0);
            gpio_set_value(led_wifi_blue, 0);
            usleep(200 * 1000);
            gpio_set_value(led_camera_blue, 1);
            gpio_set_value(led_wifi_blue, 1);
            usleep(200 * 1000);
        }

        if(net_connecting)
        {
            gpio_set_value(led_wifi_blue, 1);
            gpio_set_value(led_wifi_red, 1);
            gpio_set_value(led_wifi_green, 0);
            usleep(200 * 1000);
            gpio_set_value(led_wifi_green, 1);
            usleep(200 * 1000);
        }

        if(system_reset)
        {
            time_t t = time(0);
            while( (t - time(0)) <= 2)
            {
                gpio_set_value(led_wifi_green, 0);
                gpio_set_value(led_camera_green, 0);
                usleep(200 * 1000);
                gpio_set_value(led_wifi_green, 1);
                gpio_set_value(led_camera_green, 1);
                usleep(200 * 1000);
            }
            board_led_all_off();
        }
    }

    return NULL;
}

int board_gpio_init()
{
    gpio0 = gpiod_chip_open_by_name("gpiochip0");
    gpio2 = gpiod_chip_open_by_name("gpiochip2");
    gpio5 = gpiod_chip_open_by_name("gpiochip5");
    gpio8 = gpiod_chip_open_by_name("gpiochip8");
    gpio9 = gpiod_chip_open_by_name("gpiochip9");
    gpio10 = gpiod_chip_open_by_name("gpiochip10");

    /* button */
    btn_mute = gpiod_chip_get_line(gpio10, 6);
    gpiod_line_request_both_edges_events(btn_mute, "HxtDeskService");
    btn_reset = gpiod_chip_get_line(gpio5, 7);
    gpiod_line_request_both_edges_events(btn_reset, "HxtDeskService");
    btn_vol_inc = gpiod_chip_get_line(gpio8, 2);
    gpiod_line_request_both_edges_events(btn_vol_inc, "HxtDeskService");
    btn_vol_dec = gpiod_chip_get_line(gpio8, 4);
    gpiod_line_request_both_edges_events(btn_vol_dec, "HxtDeskService");

    /* led */
    led_wifi_red = gpiod_chip_get_line(gpio0, 1);
    gpiod_line_request_output(led_wifi_red, "HxtDeskService", GPIOD_LINE_ACTIVE_STATE_HIGH);
    led_wifi_blue = gpiod_chip_get_line(gpio2, 4);
    gpiod_line_request_output(led_wifi_blue, "HxtDeskService", GPIOD_LINE_ACTIVE_STATE_HIGH);
    led_wifi_green = gpiod_chip_get_line(gpio9, 2);
    gpiod_line_request_output(led_wifi_green, "HxtDeskService", GPIOD_LINE_ACTIVE_STATE_HIGH);

    led_camera_blue = gpiod_chip_get_line(gpio2, 5);
    gpiod_line_request_output(led_camera_blue, "HxtDeskService", GPIOD_LINE_ACTIVE_STATE_HIGH);
    led_camera_red = gpiod_chip_get_line(gpio2, 6);
    gpiod_line_request_output(led_camera_red, "HxtDeskService", GPIOD_LINE_ACTIVE_STATE_HIGH);
    led_camera_green = gpiod_chip_get_line(gpio2, 7);
    gpiod_line_request_output(led_camera_green, "HxtDeskService", GPIOD_LINE_ACTIVE_STATE_HIGH);

    led_mute_red = gpiod_chip_get_line(gpio9, 4);
    gpiod_line_request_output(led_mute_red, "HxtDeskService", GPIOD_LINE_ACTIVE_STATE_HIGH);
    led_mute_blue = gpiod_chip_get_line(gpio0, 5);
    gpiod_line_request_output(led_mute_blue, "HxtDeskService", GPIOD_LINE_ACTIVE_STATE_HIGH);
    led_mute_green = gpiod_chip_get_line(gpio10, 1);
    gpiod_line_request_output(led_mute_green, "HxtDeskService", GPIOD_LINE_ACTIVE_STATE_HIGH);


    /* button event */
    pthread_t t1, t2, t3, t4, t5;
    pthread_create(&t1, NULL, check_inc_vol_event, NULL);
    pthread_create(&t2, NULL, check_dec_vol_event, NULL);
    pthread_create(&t3, NULL, check_scan_qrcode_event, NULL);
    pthread_create(&t4, NULL, check_posture_event, NULL);
    pthread_create(&t5, NULL, check_reset_event, NULL);
    board_listen_posture_event();
    board_led_blinking();

    return 0;
}

void board_gpio_uninit()
{
    gpiod_line_release(btn_reset);
    gpiod_line_release(btn_mute);
    gpiod_line_release(btn_vol_inc);
    gpiod_line_release(btn_vol_dec);
    gpiod_line_release(led_camera_green);
    gpiod_line_release(led_camera_blue);
    gpiod_line_release(led_camera_red);
    gpiod_line_release(led_wifi_green);
    gpiod_line_release(led_wifi_blue);
    gpiod_line_release(led_wifi_red);
    gpiod_line_release(led_mute_green);
    gpiod_line_release(led_mute_blue);
    gpiod_line_release(led_mute_red);

    gpiod_chip_close(gpio0);
    gpiod_chip_close(gpio2);
    gpiod_chip_close(gpio5);
    gpiod_chip_close(gpio8);
    gpiod_chip_close(gpio9);
    gpiod_chip_close(gpio10);

    return;
}

void board_led_blinking()
{
    pthread_t tid; 
    pthread_create(&tid, NULL, led_blinking_thread, NULL);
}

void board_stop_boot_led_blinking()
{
    system_booting = FALSE;

    gpio_set_value(led_camera_blue, 1);
    gpio_set_value(led_camera_green, 0);
    gpio_set_value(led_wifi_blue, 0);
}

void board_start_connect_led_blinking()
{
    net_connecting = TRUE;
}

void board_stop_connect_led_blinking()
{
    net_connecting = FALSE;

    gpio_set_value(led_wifi_blue, 1);
    gpio_set_value(led_wifi_red, 1);
    gpio_set_value(led_wifi_green, 0);
}

void board_start_reset_led_blinking()
{
    system_reset = TRUE;
}

void board_listen_posture_event()
{
    pthread_t tid;
    pthread_create(&tid, NULL, check_posture_event, NULL);
}

void board_led_camera_error()
{
    gpio_set_value(led_camera_green, 1);
    gpio_set_value(led_camera_blue, 1);
    gpio_set_value(led_camera_red, 0);
}

void board_led_wifi_error()
{
    gpio_set_value(led_wifi_green, 1);
    gpio_set_value(led_wifi_blue, 1);
    gpio_set_value(led_wifi_red, 0);
}

void board_led_all_off()
{
    gpio_set_value(led_wifi_green, 1);
    gpio_set_value(led_wifi_blue, 1);
    gpio_set_value(led_wifi_red, 1);

    gpio_set_value(led_camera_green, 1);
    gpio_set_value(led_camera_blue, 1);
    gpio_set_value(led_camera_red, 1);

    gpio_set_value(led_mute_green, 1);
    gpio_set_value(led_mute_blue, 1);
    gpio_set_value(led_mute_red, 1);
}

BOOL board_get_qrcode_scan_status()
{
    return qrcode_scanning;
}
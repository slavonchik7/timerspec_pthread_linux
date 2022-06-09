

#ifndef SIGTIMERVAL_H
#define SIGTIMERVAL_H 1

#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>


/* тип для хранения и использования миллисеккунд
 * при инициализации таймиера */
#define tmspc_mseconds double


/* возможниые значения статуса таймера __tm_status */
#define TM_NOT_SETTED   511
#define TM_BROKEN       512
#define TM_GOING        513
#define TM_FINISHED     514
#define TM_STOP         515
#define TM_SETTED       516
#define TM_PAUSE        517
#define TM_WAIT_CMD     518
#define TM_NO_STATUS    519

/* возможниые значения статуса ошибки tm_error */
#define ST_OK       521
#define SET_ERROR   522
#define STOP_ERROR  523

/* возможные значения __s_purpose_notify,
 * цели, с которыми может быть отправлен сигнал */
#define _NOTIFY_TM_PAUSE    531
#define _NOTIFY_TM_GOING    532
#define _NOTIFY_TM_BREAK    533
#define _NOTIFY_TM_RESTART  534
#define _NOTIFY_TM_STOP     535
#define _NO_NOTIFY          536

/* возможные значения __exec_status,
 * которая показывает, готов ли таймер выполнить и обработать новую операцию */
#define _TM_AVAILABLE   541
#define _TM_BUSY        542

/* возможные значения поля __proc_pselect,
 * которое показывает, завершила ли свою работу функция pselect */
#define _PS_WORK        551
#define _PS_FREE        552


struct mute_cond_tm {
    pthread_mutex_t mute_lock;
    pthread_cond_t val_cond;
};

struct wtime_s {
    struct timespec control_wtime_value;
    struct timespec save_wtime_value;

    struct timespec control_wtime_interval;
    struct timespec save_wtime_interval;
};

#if 0
struct tm_mutex_vars {
    struct mute_cond_tm __w_last_oper;
    struct mute_cond_tm __w_ps_finish;

    pthread_mutex_t __t_flag_lock;

    pthread_mutex_t __clock_delta_block;
};
#endif // 0


/* структура описывает текущие настройки таймера
* func_sig_handler_ptr - функцию обработки,
* sig_h_data - данные, передаваемые в неё,
* __control_wtime - настройки таймера timespec,
* __save_wtime - сохранение настроек таймера timespec,
* __tmspec_tid - id потока,
* tm_error - последняя ошибка в функциях таймера,
* __s_purpose_notify - цель, с которой был отправлен сигнал
* __tm_status - текущий статус таймера,
* __sset_blocked - сигналы, заблокированные в потоке выполнения таймера
*       и функции обработчика
* ВНИМАНИЕ!!! переменные, начинающиеся с "__" не следует изменять,
* использовать их стоит только для чтения, в противном случае возможны ошибки */
struct sigtimerspec {
    struct wtime_s __wtime_settings;

    int __interval_count;

    pthread_t __tmspec_tid;
    sigset_t __sset;

    void *(*func_sig_handler_ptr)(void *);
    void *sig_h_data;

    pthread_mutex_t __t_flag_lock;

    volatile int __s_purpose_notify;
    volatile int __tm_status;

    volatile int __exec_status;
    volatile int __proc_pselect;

    struct mute_cond_tm __w_last_oper;
    struct mute_cond_tm __w_ps_finish;

    int tm_error;
};



struct sigtimerspec *set_sigtimerspec(tmspc_mseconds value_msec,
                        tmspc_mseconds interval_msec,
                        void *(*func_sig_handler)(void *),
                        void *func_sh_data);

int reset_sigtimerspec_full(struct sigtimerspec *s_tmvl,
                            tmspc_mseconds value_msec,
                            tmspc_mseconds interval_msec,
                            void *(*func_sig_handler)(void *),
                            void *func_sh_da);

int reset_sigtimerspec_tm_only(struct sigtimerspec *s_tmvl,
                        tmspc_mseconds value_msec,
                        tmspc_mseconds interval_msec);

int unset_sigtimerspec(struct sigtimerspec *s_tmvl);

/* продолжить таймер (после паузы) */
int resume_sigtimerspec(struct sigtimerspec *s_tmvl);

/* пауза (предполагается, таймер перед этим запущен) */
int pause_sigtimerspec(struct sigtimerspec *s_tmvl);

/* запуск таймера */
int start_sigtimerspec(struct sigtimerspec *s_tmvl);

/* остановка таймера */
int stop_sigtimerspec(struct sigtimerspec *s_tmvl);

/* ожидание истечения таймера */
int wait_sigtimerspec(struct sigtimerspec *s_tmvl, int event_num);

#endif  /* SIGTIMERVAL_H */

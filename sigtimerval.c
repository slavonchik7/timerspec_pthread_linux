

#include "sigtimerval.h"





/*
    сделать функцию ожидания завершения предыдущей операции таймера, использовав
        pthread_cond_broadcast();
        pthread_cond_wait();

    при помощи их же переписать функцию
        static void __wait_sig_conf(sigset_t *sset);
        сделать её универсальной, добавив в struct sigtimerspec поле состояния,
        которое показывает, закончена ли предыдущая операция

    почему блокировка внутри функции __waiting_pselect_finished не блокирует изменение параметров
        и пробуждение после завешения pselect? даже, если __waiting_pselect_finished вызывается до pthread_kill

    доработать структуру tm_mutex_vars и пересмотреть работу с блокировками задействовать


    вначале каждой функции ..._sigtimerspec вызывается функция __waiting_last_timer_operation_finish
        на случай, если функция вызывается, а таймер завершится, то следует подождать, пока таймер
        не перенастроится и не завершится пользовательская функция обработки
        в противном случае могут возникнуть ошибки


    была ошибка в функциях __waiting_pselect_finished и __waiting_last_timer_operation_finish, из-за блокировках в них
    они работали некорректно, они вызывались в функции wait_sigtimerspec и при малом времени (примерно 1 мксек),
        __waiting_pselect_finished не могла поймать выход из фукнции pselect,
        а __waiting_last_timer_operation_finish не ждала, пока завершится перенастройка, в связи с чем
        они обе не реагировали на срабатывание таймера и поэтому таймера срабатывал гораздо больше раз, чем
        было указано во входном значении функции <сейчас ошибка исправлена>


*/

/* сигнал используется, для уведомления потока о том, что произошёл
 * запрос на совершение какого-либо действия по отношению к таймеру
 * поставить на паузу, продолжить, уничтожить счётчик и выйти из потока */
static volatile int _SIG_TM_NOTIFY = SIGUSR1;


static void *__control_pthread_handler(void *v_data);

static void __break_timer(struct sigtimerspec *s_tmvl);

static void __tm_mscnds_to_tmspec(struct timespec *tms, tmspc_mseconds value_msec);

static void __sig_timer_handler(int snum, siginfo_t *sinfo, void *s_data);

static void __waiting_last_timer_operation_finish(struct sigtimerspec *s_tmvl);

static void __waiting_pselect_finished(struct sigtimerspec *s_tmvl);


static void __set_time(struct sigtimerspec *s_tmvl,
                        tmspc_mseconds value_msec, tmspc_mseconds interval_msec);

static void __recalculation_time(struct timespec *recalc_tm, struct timespec *start_tm, struct timespec *end_tm);


/* --- НЕ ИСПОЛЬЗУЮТСЯ --- */

/* ожидание установки сигналов */
static void __wait_sig_conf(sigset_t *sset);

/* ожидание перенастройки таймера и его срабатывания */
static void __wait_timer_is_ready(struct sigtimerspec *s_tmvl);

/* --- НЕ ИСПОЛЬЗУЮТСЯ --- */


struct sigtimerspec *set_sigtimerspec(tmspc_mseconds value_msec,
                        tmspc_mseconds interval_msec,
                        void *(*func_sig_handler)(void *),
                        void *func_sh_data) {

    struct sigtimerspec *sigtmval = (struct sigtimerspec *)malloc(sizeof(struct sigtimerspec));
    if (sigtmval == NULL)
        return NULL;

    /* инициализирую переменную блокировки, для корректного обращения к
     * имеющим спецификатор volatile,
     * обращение к этой переменной происходит, как внутри потока таймера, так и из главного потока */
    pthread_mutex_init(&sigtmval->__t_flag_lock, NULL);

    pthread_mutex_init(&sigtmval->__w_last_oper.mute_lock, NULL);
    pthread_mutex_init(&sigtmval->__w_ps_finish.mute_lock, NULL);


    /* инициализирую переменную, для возможности координировать действия
     * между главным потоком и потоком таймера */
    pthread_cond_init(&sigtmval->__w_last_oper.val_cond, NULL);
    pthread_cond_init(&sigtmval->__w_ps_finish.val_cond, NULL);

    /* начальное значение для счётчика количества интервалов таймера
     * не 0 потому что, после 1 срабатывания по значению control_wtime_value счётчик
     * станет равен 0, что является корректным значением
     * такое решение в дальнейшем будет удобно для функции wait_sigtimerspec */
    sigtmval->__interval_count = -1;

    /* блокируем таймер, так как он в данный момент настраивается */
    sigtmval->__exec_status = _TM_BUSY;

    /* установка началного значения поля ошибки */
    sigtmval->tm_error = ST_OK;

    /* установка текущего статуса таймера */
    sigtmval->__tm_status = TM_NOT_SETTED;

    /* перевод из милисекунд в структуру timespec */
    __set_time(sigtmval, value_msec, interval_msec);

    sigtmval->func_sig_handler_ptr = func_sig_handler;    /* устанавливаю обработчик окончания таймера */
    sigtmval->sig_h_data = func_sh_data;    /* устанавливаю данные, которые будут переданы в обработчик */


    /* создание потока таймера */
    pthread_create(&sigtmval->__tmspec_tid, NULL, __control_pthread_handler, sigtmval);

    __waiting_last_timer_operation_finish(sigtmval);

    return sigtmval;
}


/* будет ждать пока таймер сработает event_num число раз
 * после чего вернёт 0, в случае ошибки вернёт -1 */
int wait_sigtimerspec(struct sigtimerspec *s_tmvl, int event_num) {
    if (s_tmvl == NULL)
        return -1;

    if (event_num < 0)
        return -1;

    __waiting_last_timer_operation_finish(s_tmvl);

    if (s_tmvl->__tm_status == TM_GOING) {
        while (event_num-- > 0) {
            __waiting_pselect_finished(s_tmvl);
            __waiting_last_timer_operation_finish(s_tmvl);
        }

        return 0;
    }

    return -1;
}


/* полная перенайстройка таймера (только пользовательские поля данных)
 * если таймер в данный момент запущен, то он будет остановлен */
int reset_sigtimerspec_full(struct sigtimerspec *s_tmvl,
                            tmspc_mseconds value_msec,
                            tmspc_mseconds interval_msec,
                            void *(*func_sig_handler)(void *),
                            void *func_sh_da) {
    if (s_tmvl == NULL)
        return -1;

    /* остановка таймера */
    if (stop_sigtimerspec(s_tmvl) != 0)
        return -1;

    s_tmvl->func_sig_handler_ptr = func_sig_handler;
    s_tmvl->sig_h_data = func_sh_da;

    __set_time(s_tmvl, value_msec, interval_msec);

    return 0;
}


/* перенайстройка таймера (только поле времени ожидания)
 * если таймер в данный момент запущен, то он будет остановлен */
int reset_sigtimerspec_tm_only(struct sigtimerspec *s_tmvl,
                        tmspc_mseconds value_msec,
                        tmspc_mseconds interval_msec) {
    if (s_tmvl == NULL)
        return -1;

    /* остановка таймера */
    if (stop_sigtimerspec(s_tmvl) != 0)
        return -1;

    __set_time(s_tmvl, value_msec, interval_msec);

    return 0;
}


int unset_sigtimerspec(struct sigtimerspec *s_tmvl) {
    if (s_tmvl == NULL)
        return -1;

    /* прерывание таймера и выход из потока таймера */
    __break_timer(s_tmvl);

    /* уничтажаю переменные блокировки и ожидания */
    pthread_mutex_destroy(&s_tmvl->__t_flag_lock);

    pthread_mutex_destroy(&s_tmvl->__w_last_oper.mute_lock);
    pthread_mutex_destroy(&s_tmvl->__w_ps_finish.mute_lock);

    pthread_cond_destroy(&s_tmvl->__w_last_oper.val_cond);
    pthread_cond_destroy(&s_tmvl->__w_ps_finish.val_cond);

    free(s_tmvl);

    return 0;
}


static void __set_time(struct sigtimerspec *s_tmvl,
                            tmspc_mseconds value_msec, tmspc_mseconds interval_msec) {

    __tm_mscnds_to_tmspec(&s_tmvl->__wtime_settings.control_wtime_value, value_msec);
    s_tmvl->__wtime_settings.save_wtime_value = s_tmvl->__wtime_settings.control_wtime_value;

    __tm_mscnds_to_tmspec(&s_tmvl->__wtime_settings.control_wtime_interval, interval_msec);
    s_tmvl->__wtime_settings.save_wtime_interval = s_tmvl->__wtime_settings.control_wtime_interval;
}


static void __tm_mscnds_to_tmspec(struct timespec *tms,
                    tmspc_mseconds value_msec) {

    value_msec *= (value_msec > 0);

    tms->tv_sec = (int)value_msec / 1000;
    tms->tv_nsec = ((int)value_msec % 1000 + value_msec - (int)value_msec) * 1000000;
}


void *__control_pthread_handler(void *v_data) {

    struct sigtimerspec *st_d = (struct sigtimerspec *)v_data;

    st_d->__exec_status = _TM_BUSY;

    /* маска для pselect, он заполнит его сигналом _SIG_TN_BREAK */
    /* почему pselect сам заполняет? */
    sigset_t empty_mask;


    /* устанавливаю сигналы, которые будут перехватываться в текущем потоке */

    /* очищаю набор сигналов */
    sigemptyset(&st_d->__sset);

    /* добавляю сигналы, на которые будет реагировать поток */
    sigaddset(&st_d->__sset, _SIG_TM_NOTIFY);

    /* блокирую сигналы маски таймера */
    if (pthread_sigmask(SIG_BLOCK, &st_d->__sset, NULL) == -1) {
        st_d->tm_error = SET_ERROR;
        return NULL;
    }

    /* устанавливаю обработчики сигнала */
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = __sig_timer_handler;

    sigemptyset(&sa.sa_mask);
    if (sigaction(_SIG_TM_NOTIFY, &sa, NULL) == -1) {
        st_d->tm_error = SET_ERROR;
        return NULL;
    }


    /* очищаю маску для pselect, так как функция сама заполнит её сигналами,
     * включенными для данного потока */
    sigemptyset(&empty_mask);

    /* изменяю статус таймера на ожидание команды */
    st_d->__tm_status = TM_WAIT_CMD;
    st_d->__proc_pselect = _PS_FREE;

    /* в переменные start_point и end_point будут сохранены значения системного временми
     * до и после функции pselect, которая выполняет роль основного таймера
     * для подсчёта оставшегося времени */
    struct timespec start_point;
    struct timespec end_point;

    /* если value_time_flag == 0, то таймер сработал 1 раз
     * control_wtime_value */
    int value_time_flag = 1;

    /* указатель, на таймер, которы будет использован, в зависимости о ситуации
     * таймер первого срабатывания или таймер последующих интервалов */
    struct timespec *current_ps_wtime = NULL;
    struct timespec *save_current_ps_wtime = NULL;

    /* вначале мы ждём, пока не будет послан сигнал, то есть заходим в 1 цикл, который отвечает за паузу и ожидание
     * если получен сигнал, проверяем, с какой целью он был послан, проверяя
     * значение поля __s_purpose_notify
     * получив сигнал _NOTIFY_TM_BE_GOING (пуск таймера), меняем поле __tm_status на TM_GOING (таймер запущен) */
    while (1) {
        if (st_d->__tm_status == TM_WAIT_CMD
                || st_d->__tm_status == TM_PAUSE
                || st_d->__tm_status == TM_FINISHED) {

            while (1) {

                pthread_mutex_lock(&st_d->__w_last_oper.mute_lock);
                /* уведомляю о том, что pselect запущена
                 * после это строки кода функция __waiting_pselect_breaking
                 * не вернётся, пока не будет передан сигнал об уведомлении */
                st_d->__proc_pselect = _PS_WORK;

                /* говорю, что таймер завершил операции, и готов получать новые */
                st_d->__exec_status = _TM_AVAILABLE;    /* уведомляю о доступности таймера */

                /* пробуждаю функцию __waiting_last_timer_operation_finish */
                pthread_cond_broadcast(&st_d->__w_last_oper.val_cond);

                pthread_mutex_unlock(&st_d->__w_last_oper.mute_lock);


                /* таймер ожидания */
                pselect(0, NULL, NULL, NULL, NULL, &empty_mask);

                pthread_mutex_lock(&st_d->__w_ps_finish.mute_lock);

                /* уведомляю о блокировке таймера */
                st_d->__exec_status = _TM_BUSY;

                /* уведомляю о том, что функция pselect, получила сигнал, обработала его
                 * и завершила своё выполнение */
                st_d->__proc_pselect = _PS_FREE;
                /* пробуждаю функцию __waiting_pselect_breaking */
                pthread_cond_broadcast(&st_d->__w_ps_finish.val_cond);

                pthread_mutex_unlock(&st_d->__w_ps_finish.mute_lock);

                /* обработка запроса на выполнения отсчёта таймера, на его продолжение */
                if (st_d->__s_purpose_notify == _NOTIFY_TM_GOING) {

                    /* снимаю статус какого-либо уведомления */
                    st_d->__s_purpose_notify = _NO_NOTIFY;

                    /* уведомляю о продолжении или запуске таймера */
                    st_d->__tm_status = TM_GOING;
                    break;
                }
                /* если получено какое-то из этих уведомлений,
                 * переходим в конец основного цикла, для и обработки*/
                else if (st_d->__s_purpose_notify == _NOTIFY_TM_BREAK
                            || st_d->__s_purpose_notify == _NOTIFY_TM_STOP
                            || st_d->__s_purpose_notify == _NOTIFY_TM_RESTART) {

                    st_d->__tm_status = TM_NO_STATUS;
                    break;
                }

            }
        }



        /* выполняется в случае получения запроса на выполнения отсчёта таймера */
        if (st_d->__tm_status == TM_GOING) {
            while(1) {

                /* если первое срабатывание таймера не задано, то таймер не будет работать вовсе
                 * таймер переведё в режим ожидания
                 * если требуется, чтобы таймер сразу начинал со значения interval, то
                 * его и следует указать в value */
                if (st_d->__wtime_settings.save_wtime_value.tv_sec == 0
                        && st_d->__wtime_settings.save_wtime_value.tv_nsec == 0) {
                    st_d->__tm_status = TM_WAIT_CMD;
                    break;
                }

                /* выбираю, через сколько произойдёт следующее срабатывание */
                if (value_time_flag) {
                    current_ps_wtime = &st_d->__wtime_settings.control_wtime_value;
                    save_current_ps_wtime = &st_d->__wtime_settings.save_wtime_value;
                } else {
                    /* если таймер сработал уже 1 раз и, задыный пользователем
                     * интервал (0;0), то переводим таймер в режим ожидания */
                    if (st_d->__wtime_settings.save_wtime_interval.tv_sec == 0
                            && st_d->__wtime_settings.save_wtime_interval.tv_nsec == 0) {

                        st_d->__tm_status = TM_WAIT_CMD;
                        break;
                    }

                    current_ps_wtime = &st_d->__wtime_settings.control_wtime_interval;
                    save_current_ps_wtime = &st_d->__wtime_settings.save_wtime_interval;
                }

                pthread_mutex_lock(&st_d->__w_last_oper.mute_lock);

                /* уведомляю о том, что pselect запущена
                 * после это строки кода функция __waiting_pselect_breaking
                 * не вернётся, пока не будет передан сигнал об уведомлении */
                st_d->__proc_pselect = _PS_WORK;

                /* говорю, что таймер завершил операции, и готов получать новые */
                st_d->__exec_status = _TM_AVAILABLE;
                pthread_cond_broadcast(&st_d->__w_last_oper.val_cond);

                pthread_mutex_unlock(&st_d->__w_last_oper.mute_lock);


                /* сохраняю текущее время исходя из системных часов */
                clock_gettime(CLOCK_REALTIME, &start_point);

                pselect(0, NULL, NULL, NULL, current_ps_wtime, &empty_mask);

                /* сохраняю текущее время исходя из системных часов */
                clock_gettime(CLOCK_REALTIME, &end_point);

                pthread_mutex_lock(&st_d->__w_ps_finish.mute_lock);
                /* уведомляю о блокировке таймера */
                st_d->__exec_status = _TM_BUSY;

                /* уведомляю о том, что функция pselect, получила сигнал, обработала его
                 * и завершила своё выполнение */
                st_d->__proc_pselect = _PS_FREE;

                /* пробуждаю функцию __waiting_pselect_breaking */
                pthread_cond_broadcast(&st_d->__w_ps_finish.val_cond);

                pthread_mutex_unlock(&st_d->__w_ps_finish.mute_lock);


                /* обработка уведомления о паузе */
                if (st_d->__s_purpose_notify == _NOTIFY_TM_PAUSE) {

                    /* снимаю статус какого-либо уведомления */
                    st_d->__s_purpose_notify = _NO_NOTIFY;

                    /* высчитываю оставшееся время, для последующего запуска таймера */
                    __recalculation_time(current_ps_wtime, &start_point, &end_point);

                    /* уведомляю о том, что таймер приостановлен (стоит на паузе) */
                    st_d->__tm_status = TM_PAUSE;

                    break;
                }
                /* случай, когда никакого сигнала послано не было и время таймера истекло
                 * вызываем функцию обработки, заданную пользователем */
                else if (st_d->__s_purpose_notify == _NO_NOTIFY) {

                    /* если сработал впервые, то говорю, что в следующий
                     * раз следует засекать по значению interval */
                    if (value_time_flag)
                        value_time_flag = 0;

                    if (st_d->__wtime_settings.save_wtime_interval.tv_sec == 0
                            && st_d->__wtime_settings.save_wtime_interval.tv_nsec == 0) {

                        /* уведомляю об истечении времени таймера в случае, если задано только 1 срабатывание
                         * по значению value */
                        st_d->__tm_status = TM_FINISHED;
                    } else {
                        /* не зависимо от того, сколько раз сработал таймер
                         * далее он будет запускаться снова, а значит он не будет завершён
                         * и присвоить ему статус завершения TM_FINISHED будет не корректно
                         * программа должна после срабатывания сразу же войти обратно в этот
                         * цикл и продолжить выполнение таймера */
                        st_d->__tm_status = TM_GOING;
                    }

                    /* увеличиваю счётчик срабатывания таймера */
                    st_d->__interval_count++;

                    /* если таймер истёк, и мы попали в этот участок кода, то стоит перезаписать значения
                     * на начальные, так как до истечения времени могла быть вызвана функция
                     * pause_sigtimerspec, после которой происходит пересчёт времени */
                    current_ps_wtime = save_current_ps_wtime;

                    /* вызов основной потоковой функции */
                    if (st_d->func_sig_handler_ptr != NULL)
                        st_d->func_sig_handler_ptr(st_d->sig_h_data);

                    break;
                }
                /* если получено какое-то из этих уведомлений,
                 * переходим в конец основного цикла, для и обработки*/
                else if (st_d->__s_purpose_notify == _NOTIFY_TM_BREAK
                            || st_d->__s_purpose_notify == _NOTIFY_TM_STOP
                            || st_d->__s_purpose_notify == _NOTIFY_TM_RESTART) {

                    st_d->__tm_status = TM_NO_STATUS;
                    break;
                }
            }
        }

        /* получен сигнал уничтожения таймера
         * будет произведён выход из потоковой функции */
        if (st_d->__s_purpose_notify == _NOTIFY_TM_BREAK) {

            /* снимаю статус какого-либо уведомления */
            st_d->__s_purpose_notify = _NO_NOTIFY;

            /* уведомляю о том, что таймер прерван */
            st_d->__tm_status = TM_BROKEN;


            pthread_mutex_lock(&st_d->__w_last_oper.mute_lock);

            /* говорю, что таймер завершил операции, и готов получать новые */
            st_d->__exec_status = _TM_AVAILABLE;
            pthread_cond_broadcast(&st_d->__w_last_oper.val_cond);

            pthread_mutex_unlock(&st_d->__w_last_oper.mute_lock);

            return NULL;
        }

        if (st_d->__s_purpose_notify == _NOTIFY_TM_RESTART) {
            /* снимаю статус какого-либо уведомления */
            st_d->__s_purpose_notify = _NO_NOTIFY;

            /* начинаем снова с control_wtime_value */
            value_time_flag = 1;
            st_d->__interval_count = -1;
            /* уведомляю о продолжении или запуске таймера */
            st_d->__tm_status = TM_GOING;

            /* устанавливаю начальные настройки таймера */
            st_d->__wtime_settings.control_wtime_value = st_d->__wtime_settings.save_wtime_value;
            st_d->__wtime_settings.control_wtime_interval = st_d->__wtime_settings.save_wtime_interval;

            continue;
        }

        if (st_d->__s_purpose_notify == _NOTIFY_TM_STOP) {

            /* снимаю статус какого-либо уведомления */
            st_d->__s_purpose_notify = _NO_NOTIFY;

            /* уведомляю о том, что таймер находится в режиме ожидания */
            st_d->__tm_status = TM_WAIT_CMD;

            continue;
        }

    }

    return NULL;
}


/* функция продолжает работу таймера только в том случае, если он поставлен на паузу
 * и в этом же случае вернёт 0, также она вернёт 0, если таймер уже запущен
 * в остальных случаях вернёт -1 */
int resume_sigtimerspec(struct sigtimerspec *s_tmvl) {
    if (s_tmvl == NULL)
        return -1;

    __waiting_last_timer_operation_finish(s_tmvl);

    if (s_tmvl->__tm_status == TM_GOING)
        return 0;


    if (s_tmvl->__tm_status == TM_PAUSE) {

        s_tmvl->__s_purpose_notify = _NOTIFY_TM_GOING;

        pthread_kill(s_tmvl->__tmspec_tid, _SIG_TM_NOTIFY);

        __waiting_pselect_finished(s_tmvl);

        /* ждём пока таймер станет доступен */
        __waiting_last_timer_operation_finish(s_tmvl);

        return 0;
    }

    return -1;
}

/* функция ставит таймер на паузу, независимо от того, какой счётчик сейчас в работе
 * value или interval, в случае, если таймер уже на паузе, функция вернёт 0 (корректное завершение)
 * если таймер сейчас в работе, то ставим на пауз и возвращаем 0
 * в любых других случаях вернёт -1 */
int pause_sigtimerspec(struct sigtimerspec *s_tmvl) {
    if (s_tmvl == NULL)
        return -1;

    __waiting_last_timer_operation_finish(s_tmvl);

    /* если таймер уже на паузе, выходим */
    if (s_tmvl->__tm_status == TM_PAUSE)
        return 0;


    if (s_tmvl->__tm_status == TM_GOING) {

        s_tmvl->__s_purpose_notify = _NOTIFY_TM_PAUSE;

        pthread_kill(s_tmvl->__tmspec_tid, _SIG_TM_NOTIFY);

        __waiting_pselect_finished(s_tmvl);

        /* ждём пока таймер станет доступен */
        __waiting_last_timer_operation_finish(s_tmvl);

        return 0;
    }

    return -1;
}

/* функция запускает таймер
 * на самом деле функция перезапускает таймер, не зависимо от того, в каком состоянии он в данный момент
 * кроме статсуса TM_NOT_SETTED, но таймер не может находится в этом состоянии, так как функция set_sigtimerspec()
 * ожидает, пока тамер не будет установлен */
int start_sigtimerspec(struct sigtimerspec *s_tmvl) {
    if (s_tmvl == NULL)
        return -1;

    __waiting_last_timer_operation_finish(s_tmvl);

    /* функция не станет работать, толко если таймер не был настроен */
    if (s_tmvl->__tm_status == TM_NOT_SETTED)
        return -1;

    s_tmvl->__s_purpose_notify = _NOTIFY_TM_RESTART;

    pthread_kill(s_tmvl->__tmspec_tid, _SIG_TM_NOTIFY);

    __waiting_pselect_finished(s_tmvl);

    /* ждём пока таймер станет доступен */
    __waiting_last_timer_operation_finish(s_tmvl);

    return 0;
}


/* функция полность останвливает работу таймера
 * после вызова этой функции можно нельзя будет воспользоваться функцией resume_sigtimerspec()
 * для возобновления работы таймера
 * он может быть запущен только функцией start_sigtimerspec()
 * функция вернёт -1, толко в случае, если таймер не настроен(не установлен)
 * в остальных случаях она выдаст 0(завершится верно) */
int stop_sigtimerspec(struct sigtimerspec *s_tmvl) {
    if (s_tmvl == NULL)
        return -1;

    __waiting_last_timer_operation_finish(s_tmvl);

    /* функция не станет работать, толко если таймер не был настроен */
    if (s_tmvl->__tm_status == TM_NOT_SETTED)
        return -1;

    s_tmvl->__s_purpose_notify = _NOTIFY_TM_STOP;

    pthread_kill(s_tmvl->__tmspec_tid, _SIG_TM_NOTIFY);

    __waiting_pselect_finished(s_tmvl);

    /* ждём пока таймер станет доступен */
    __waiting_last_timer_operation_finish(s_tmvl);

    return 0;
}


/* функция устанавливает значение уведомления в _NOTIFY_TM_BREAK,
* посылает сигнал _SIG_TM_NOTIFY потоку, в котором выполняется таймер
* и ждёт, пока произойдёт досрочное завершение потока */
static void __break_timer(struct sigtimerspec *s_tmvl) {
    if (s_tmvl == NULL)
        return;

    __waiting_last_timer_operation_finish(s_tmvl);

    s_tmvl->__s_purpose_notify = _NOTIFY_TM_BREAK;

    pthread_kill(s_tmvl->__tmspec_tid, _SIG_TM_NOTIFY);

    __waiting_pselect_finished(s_tmvl);
    __waiting_last_timer_operation_finish(s_tmvl);

    pthread_join(s_tmvl->__tmspec_tid, NULL);
}


static void __sig_timer_handler(int snum, siginfo_t *sinfo, void *s_data) {}


static void __waiting_last_timer_operation_finish(struct sigtimerspec *s_tmvl) {

    /* если таймер уже доступен, то есть в данный момент работает функция pselect, то
     * выходим из функции */
    if (s_tmvl->__exec_status == _TM_AVAILABLE)
        return;

    /* ожидаем, пока завершится настройка таймера */
    pthread_cond_wait(&s_tmvl->__w_last_oper.val_cond, &s_tmvl->__w_last_oper.mute_lock);

}

/* функция ожидает, пока функция pselect не получит сигнал и не завершит своё выполнение
 * функция должна вызываться каждый раз, когда посылается сигнал таймеру (сразу после посылки сигнала) */
static void __waiting_pselect_finished(struct sigtimerspec *s_tmvl) {

    /* если pselect сейчас вызвана и находится в работе, ожидаем, пока она завершит своё выполнение */
    if (s_tmvl->__proc_pselect == _PS_WORK)
        /* ожидаем, пока pselect авершит своё выполнение */
        pthread_cond_wait(&s_tmvl->__w_ps_finish.val_cond, &s_tmvl->__w_ps_finish.mute_lock);

}

/* нигде не используется */
static void __wait_timer_is_ready(struct sigtimerspec *s_tmvl) {
    __waiting_pselect_finished(s_tmvl);
    __waiting_last_timer_operation_finish(s_tmvl);
}

/* нигде не используется */
static void __wait_sig_conf(sigset_t *sset) {

    while (!sigismember(sset, _SIG_TM_NOTIFY));
}

/* пересчёт времени таймера(если был поставлен на паузу) */
static void __recalculation_time(struct timespec *recalc_tm,
                        struct timespec *start_tm,
                        struct timespec *end_tm) {

    recalc_tm->tv_sec -= end_tm->tv_sec - start_tm->tv_sec;
    recalc_tm->tv_nsec -= end_tm->tv_nsec - start_tm->tv_nsec;

    recalc_tm->tv_sec *= recalc_tm->tv_sec > 0;

    if (recalc_tm->tv_nsec < 0) {
        if (recalc_tm->tv_sec > 0) {
            recalc_tm->tv_sec--;
            recalc_tm->tv_nsec = 1000000000 - abs(recalc_tm->tv_nsec);
        } else {
            recalc_tm->tv_nsec = 0;
        }
    }
}










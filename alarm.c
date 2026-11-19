#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <signal.h>

#define SERIAL_PORT "/dev/ttyACM0"
#define LOG_FILE "alarm.log"

pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile sig_atomic_t stop_requested = 0;

void handle_sigint(int signal) {
    (void)signal;
    stop_requested = 1;
}

/* Possibili stati del sistema */
typedef enum {
    SPENTO,
    ATTIVO_SAFE,
    ATTIVO_ALARM,
    ATTIVO_SILENCE
} AlarmState;

/* Eventi ricevuti da Arduino */
typedef enum {
    MOTION_DETECTED,
    MOTION_CLEARED,
    BUTTON_PRESS
} EventType;

/* Un evento da elaborare */
typedef struct {
    EventType type;
    struct timespec timestamp;
} Event;

#define QUEUE_SIZE 32

typedef struct {
    Event events[QUEUE_SIZE];
    int head;
    int tail;
    int count;

    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} EventQueue;

/* Inizializza la coda degli eventi */
void queue_init(EventQueue *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;

    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

/* Inserisce un evento nella coda */
bool queue_push(EventQueue *q, Event event) {
    pthread_mutex_lock(&q->mutex);

    /* Se la coda è piena, rifiuta l'evento */
    if (q->count == QUEUE_SIZE) {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    q->events[q->tail] = event;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;

    /* Avvisa un thread in attesa che la coda non è più vuota */
    pthread_cond_signal(&q->not_empty);

    pthread_mutex_unlock(&q->mutex);
    return true;
}

/* Estrae un evento dalla coda */
bool queue_pop(EventQueue *q, Event *event) {
    pthread_mutex_lock(&q->mutex);

    /* Aspetta finché non arriva un evento oppure viene richiesta la terminazione */
    while (q->count == 0 && !stop_requested) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    /* Se è stata richiesta la terminazione, esce */
    if (stop_requested) {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    /* Estrae l'evento */
    *event = q->events[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;

    pthread_mutex_unlock(&q->mutex);

    return true;
}

/* Azioni richieste sugli attuatori */
typedef struct {
    bool sensor_enabled;
    bool lights_on;
    bool siren_on;
} ActuatorState;


/* Traduce lo stato dell'allarme nelle azioni richieste */
ActuatorState get_actuator_state(AlarmState state) {
    ActuatorState actions = {false, false, false};

    switch (state) {
        case SPENTO:
            break;

        case ATTIVO_SAFE:
            actions.sensor_enabled = true;
            break;

        case ATTIVO_ALARM:
            actions.sensor_enabled = true;
            actions.lights_on = true;
            actions.siren_on = true;
            break;

        case ATTIVO_SILENCE:
            actions.sensor_enabled = true;
            actions.lights_on = true;
            break;
    }

    return actions;
}

/* Dati condivisi tra i thread */
typedef struct {
    AlarmState alarm_state;
    ActuatorState actuators;

    pthread_mutex_t mutex;
} AlarmSystem;

typedef struct {
    AlarmSystem *system;
} MonitorThreadArgs;

typedef struct {
    AlarmSystem *system;
} LoggerThreadArgs;

typedef struct {
    EventQueue *queue;
    AlarmSystem *system;
    int serial_fd;
} AlarmThreadArgs;

/* Inizializza il sistema nello stato SPENTO */
void alarm_system_init(AlarmSystem *system) {
    system->alarm_state = SPENTO;
    system->actuators = get_actuator_state(SPENTO);

    pthread_mutex_init(&system->mutex, NULL);
}

/* Restituisce il nuovo stato in base all'evento ricevuto */
AlarmState update_alarm_state(AlarmState state, EventType event) {
    switch (state) {

        case SPENTO:
            if (event == BUTTON_PRESS) {
                return ATTIVO_SAFE;
            }
            break;

        case ATTIVO_SAFE:
            if (event == MOTION_DETECTED) {
                return ATTIVO_ALARM;
            }
            if (event == BUTTON_PRESS) {
                return SPENTO;
            }
            break;

        case ATTIVO_ALARM:
            if (event == BUTTON_PRESS) {
                return ATTIVO_SILENCE;
            }
            break;

        case ATTIVO_SILENCE:
            if (event == MOTION_DETECTED) {
                return ATTIVO_ALARM;
            }
            if (event == BUTTON_PRESS) {
                return SPENTO;
            }
            break;
    }

    /* Se non è prevista una transizione, lo stato resta invariato */
    return state;
}

/* Elabora un evento e aggiorna lo stato del sistema */
bool alarm_system_process_event(AlarmSystem *system, Event event) {
    pthread_mutex_lock(&system->mutex);

    AlarmState old_state = system->alarm_state;

    system->alarm_state = update_alarm_state(system->alarm_state, event.type);

    bool state_changed = (old_state != system->alarm_state);

    if (state_changed) {
        system->actuators = get_actuator_state(system->alarm_state);
    }

    pthread_mutex_unlock(&system->mutex);

    return state_changed;
}

/* Legge una copia coerente dello stato del sistema */
void alarm_system_get_snapshot(
    AlarmSystem *system,
    AlarmState *state,
    ActuatorState *actuators
) {
    pthread_mutex_lock(&system->mutex);

    *state = system->alarm_state;
    *actuators = system->actuators;

    pthread_mutex_unlock(&system->mutex);
}

/* Restituisce il nome testuale dello stato */
const char *alarm_state_to_string(AlarmState state) {
    switch (state) {
        case SPENTO:
            return "SPENTO";

        case ATTIVO_SAFE:
            return "ATTIVO_SAFE";

        case ATTIVO_ALARM:
            return "ATTIVO_ALARM";

        case ATTIVO_SILENCE:
            return "ATTIVO_SILENCE";
    }

    return "STATO_SCONOSCIUTO";
}

/* Apre e configura la porta seriale */
int serial_open(void) {
    int fd = open(SERIAL_PORT, O_RDWR | O_NOCTTY);

    if (fd == -1) {
        perror("Errore apertura porta seriale");
        return -1;
    }

    struct termios tty;

    if (tcgetattr(fd, &tty) != 0) {
        perror("Errore lettura configurazione seriale");
        close(fd);
        return -1;
    }

    cfsetispeed(&tty, B9600);   // Velocità di ricezione
    cfsetospeed(&tty, B9600);   // Velocità di trasmissione

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag |= CLOCAL | CREAD;

    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("Errore configurazione porta seriale");
        close(fd);
        return -1;
    }

    return fd;  // Se tutte le op sono andate a buon fine, restituiamo il file descriptor
}

/* Invia un comando testuale ad Arduino */
bool serial_send(int fd, const char *command) {
    char buffer[64];

    int len = snprintf(buffer, sizeof(buffer), "%s\n", command);

    if (len < 0 || (size_t)len >= sizeof(buffer)) {
        fprintf(stderr, "Comando seriale troppo lungo\n");
        return false;
    }

    int total_sent = 0;

    while (total_sent < len) {
        ssize_t n = write(
            fd,
            buffer + total_sent,
            (size_t)(len - total_sent)
        );

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("Errore invio comando seriale");
            return false;
        }

        if (n == 0) {
            fprintf(stderr, "Nessun dato inviato alla porta seriale\n");
            return false;
        }

        total_sent += (int)n;
    }

    return true;
}

void apply_actuators(int serial_fd, ActuatorState actuators) {

    serial_send(serial_fd, actuators.lights_on ? "LIGHTS,1" : "LIGHTS,0");

    serial_send(serial_fd, actuators.siren_on ? "SIREN,1" : "SIREN,0");
}

/* Legge una riga dalla porta seriale */
bool serial_read_line(int fd, char *buffer, size_t size) {
    size_t pos = 0;

    while (pos < size - 1 && !stop_requested) {
        char c;
        ssize_t n = read(fd, &c, 1);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("Errore lettura seriale");
            return false;
        }

        if (n == 0) {
            if (pos == 0) {
                return false;
            }
            continue;
        }

        if (c == '\n') {
            buffer[pos] = '\0';
            return true;
        }

        if (c != '\r') {
            buffer[pos++] = c;
        }
    }

    buffer[pos] = '\0';
    return false;
}

/* Converte un messaggio Arduino in un evento interno */
bool parse_serial_event(const char *line, Event *event) {
    if (strcmp(line, "MOTION,1") == 0) {
        event->type = MOTION_DETECTED;
    } else if (strcmp(line, "MOTION,0") == 0) {
        event->type = MOTION_CLEARED;
    } else if (strcmp(line, "BUTTON,PRESS") == 0) {
        event->type = BUTTON_PRESS;
    } else {
        return false;
    }

    clock_gettime(CLOCK_MONOTONIC, &event->timestamp);
    return true;
}

void log_message(const char *message) {
    pthread_mutex_lock(&log_mutex);

    FILE *file = fopen(LOG_FILE, "a");

    if (file != NULL) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);

        struct tm time_info;
        localtime_r(&ts.tv_sec, &time_info);

        char timestamp[32];
        strftime(
            timestamp,
            sizeof(timestamp),
            "%Y-%m-%d %H:%M:%S",
            &time_info
        );

        fprintf(
            file,
            "[%s.%03ld] %s\n",
            timestamp,
            ts.tv_nsec / 1000000,
            message
        );

        fclose(file);
    } else {
        perror("Errore apertura file di log");
    }

    pthread_mutex_unlock(&log_mutex);
}

/* Thread T1: comunicazione seriale con Arduino */
typedef struct {
    int serial_fd;
    EventQueue *queue;
} SerialThreadArgs;

void *serial_thread(void *arg) {
    SerialThreadArgs *args = (SerialThreadArgs *)arg;
    char line[128]; // buffer in cui verranno salvate le righe ricevute da Arduino

    while (!stop_requested) {

        if (!serial_read_line(args->serial_fd, line, sizeof(line))) {
            continue;
        }

        Event event;

        if (parse_serial_event(line, &event)) {

            if (!queue_push(args->queue, event)) {
                fprintf(stderr, "Coda eventi piena: evento perso\n");
            }
        }
    }

    printf("[T1] Thread seriale terminato.\n");

    return NULL;
}

void *alarm_thread(void *arg) {
    AlarmThreadArgs *args = (AlarmThreadArgs *)arg;

    while (1) {
        Event event;

        if (!queue_pop(args->queue, &event)) {
            break;
        }

        bool changed = alarm_system_process_event(
            args->system,
            event
        );

        if (changed) {
            AlarmState state;
            ActuatorState actuators;

            alarm_system_get_snapshot(
                args->system,
                &state,
                &actuators
            );

            printf(
                "[ALARM] Nuovo stato: %s\n",
                alarm_state_to_string(state)
            );

            apply_actuators(args->serial_fd, actuators);
        }
    }

    printf("[T2] Thread alarm terminato.\n");

    return NULL;
}

void *monitor_thread(void *arg) {
    MonitorThreadArgs *args = (MonitorThreadArgs *)arg;

    while (!stop_requested) {
        AlarmState state;
        ActuatorState actuators;

        alarm_system_get_snapshot(
            args->system,
            &state,
            &actuators
        );

        printf(
            "[MONITOR] Stato: %s | Sensore: %s | Luci: %s | Sirena: %s\n",
            alarm_state_to_string(state),
            actuators.sensor_enabled ? "ON" : "OFF",
            actuators.lights_on ? "ON" : "OFF",
            actuators.siren_on ? "ON" : "OFF"
        );

        sleep(2);
    }

    printf("[T3] Thread monitor terminato.\n");

    return NULL;
}

void *logger_thread(void *arg) {
    LoggerThreadArgs *args = (LoggerThreadArgs *)arg;

    AlarmState previous_state;
    ActuatorState previous_actuators;

    char message[256];

    struct timespec siren_start;
    bool siren_was_on = false;

    // Legge lo stato iniziale
    alarm_system_get_snapshot(
        args->system,
        &previous_state,
        &previous_actuators
    );

    snprintf(
        message,
        sizeof(message),
        "Stato=%s | Sensore=%s | Luci=%s | Sirena=%s",
        alarm_state_to_string(previous_state),
        previous_actuators.sensor_enabled ? "ON" : "OFF",
        previous_actuators.lights_on ? "ON" : "OFF",
        previous_actuators.siren_on ? "ON" : "OFF"
    );

    log_message(message);

    // Memorizza l'eventuale attivazione iniziale della sirena
    if (previous_actuators.siren_on) {
        clock_gettime(CLOCK_MONOTONIC, &siren_start);
        siren_was_on = true;
    }

    while (!stop_requested) {
        sleep(2);

        if (stop_requested) {
            break;
        }

        AlarmState current_state;
        ActuatorState current_actuators;

        alarm_system_get_snapshot(
            args->system,
            &current_state,
            &current_actuators
        );

        // Controlla se è cambiato lo stato o almeno un attuatore
        bool changed =
            current_state != previous_state ||
            current_actuators.sensor_enabled != previous_actuators.sensor_enabled ||
            current_actuators.lights_on != previous_actuators.lights_on ||
            current_actuators.siren_on != previous_actuators.siren_on;

        if (changed) {
            snprintf(
                message,
                sizeof(message),
                "Stato=%s | Sensore=%s | Luci=%s | Sirena=%s",
                alarm_state_to_string(current_state),
                current_actuators.sensor_enabled ? "ON" : "OFF",
                current_actuators.lights_on ? "ON" : "OFF",
                current_actuators.siren_on ? "ON" : "OFF"
            );

            log_message(message);

            // La sirena è stata attivata
            if (current_actuators.siren_on && !siren_was_on) {
                clock_gettime(CLOCK_MONOTONIC, &siren_start);
                log_message("Sirena ATTIVATA");
                siren_was_on = true;
            }

            // La sirena è stata disattivata
            else if (!current_actuators.siren_on && siren_was_on) {
                struct timespec siren_end;
                clock_gettime(CLOCK_MONOTONIC, &siren_end);

                double duration =
                    (siren_end.tv_sec - siren_start.tv_sec) +
                    (siren_end.tv_nsec - siren_start.tv_nsec) / 1000000000.0;

                snprintf(
                    message,
                    sizeof(message),
                    "Sirena DISATTIVATA | Durata: %.3f secondi",
                    duration
                );

                log_message(message);
                siren_was_on = false;
            }

            previous_state = current_state;
            previous_actuators = current_actuators;
        }
    }

    printf("[T4] Thread logger terminato.\n");

    return NULL;
}


int main(void) {

    // 1. Apertura comunicazione seriale
    int serial_fd = serial_open();

    if (serial_fd < 0) {
        fprintf(stderr, "Errore apertura porta seriale\n");
        return 1;
    }

    printf("[MAIN] Comunicazione seriale aperta.\n");


    // 2. Registrazione del gestore di Ctrl+C
    struct sigaction sa;

    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Errore registrazione SIGINT");
        close(serial_fd);
        return 1;
    }


    // 3. Inizializzazione sistema e coda
    AlarmSystem system;
    EventQueue queue;

    alarm_system_init(&system);
    queue_init(&queue);

    printf("[MAIN] Sistema e coda inizializzati.\n");


    // 4. Invio configurazione iniziale ad Arduino
    AlarmState state;
    ActuatorState actuators;

    alarm_system_get_snapshot(&system, &state, &actuators);
    apply_actuators(serial_fd, actuators);

    printf("[MAIN] Configurazione iniziale: %s\n",
           alarm_state_to_string(state));


    // 5. Preparazione argomenti dei thread
    // T1: Serial Thread - deve leggere dalla porta seriale e inserire gli eventi nella coda.
    SerialThreadArgs serial_args = {
        .serial_fd = serial_fd,
        .queue = &queue
    };

    // T2: Alarm Thread - deve leggere gli eventi dalla coda, aggiornare il sistema e comunicare con Arduino.
    AlarmThreadArgs alarm_args = {
        .queue = &queue,
        .system = &system,
        .serial_fd = serial_fd
    };

    // T3: Monitor Thread - deve conoscere lo stato del sistema per monitorarlo.
    MonitorThreadArgs monitor_args = {
        .system = &system
    };

    // T4: Logger Thread - deve conoscere lo stato del sistema per svolgere il proprio lavoro.
    LoggerThreadArgs logger_args = {
        .system = &system
    };


    // 6. Dichiarazione identificativi dei thread
    pthread_t tid_serial;   // T1
    pthread_t tid_alarm;    // T2
    pthread_t tid_monitor;  // T3
    pthread_t tid_logger;   // T4


    // 7. Creazione dei quattro thread
    if (pthread_create(&tid_serial, NULL,
                       serial_thread, &serial_args) != 0) {
        fprintf(stderr, "Errore creazione T1\n");
        return 1;
    }

    if (pthread_create(&tid_alarm, NULL,
                       alarm_thread, &alarm_args) != 0) {
        fprintf(stderr, "Errore creazione T2\n");
        return 1;
    }

    if (pthread_create(&tid_monitor, NULL,
                       monitor_thread, &monitor_args) != 0) {
        fprintf(stderr, "Errore creazione T3\n");
        return 1;
    }

    if (pthread_create(&tid_logger, NULL,
                       logger_thread, &logger_args) != 0) {
        fprintf(stderr, "Errore creazione T4\n");
        return 1;
    }

    printf("[MAIN] Tutti i thread sono stati avviati.\n");


    // 8. Attesa della richiesta di terminazione
    while (!stop_requested) {
        sleep(1);
    }

    printf("\n[MAIN] Richiesta di terminazione ricevuta.\n");

    /* Risveglia T2 se è in attesa sulla coda */
    pthread_mutex_lock(&queue.mutex);
    pthread_cond_broadcast(&queue.not_empty);
    pthread_mutex_unlock(&queue.mutex);

    /* Attende la terminazione dei quattro thread */
    pthread_join(tid_serial, NULL);
    pthread_join(tid_alarm, NULL);
    pthread_join(tid_monitor, NULL);
    pthread_join(tid_logger, NULL);

    printf("[MAIN] Tutti i thread sono terminati.\n");

    /* Spegnimento di LED e buzzer prima di chiudere la seriale */
    serial_send(serial_fd, "LIGHTS,0");
    serial_send(serial_fd, "SIREN,0");

    printf("[MAIN] LED e buzzer spenti.\n");

    /* Chiusura della comunicazione seriale */
    close(serial_fd);

    /* Distruzione delle strutture di sincronizzazione */
    pthread_mutex_destroy(&queue.mutex);
    pthread_cond_destroy(&queue.not_empty);
    pthread_mutex_destroy(&system.mutex);
    pthread_mutex_destroy(&log_mutex);

    printf("[MAIN] Risorse rilasciate correttamente.\n");

    return 0;
}

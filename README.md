# MultithreadAlarmSystem
A multithreaded home alarm system developed in C using POSIX threads (pthreads) on Linux, with Arduino hardware integration. The system detects motion, manages alarm states, controls LEDs and a buzzer and records events in a log file. The project demonstrates concurrent programming, thread synchronization, and hardware-software interaction.

## Struttura del progetto

* **`codice.ino`** — Sketch principale per Arduino: gestisce l'inizializzazione dei pin (`setup`), il ciclo principale (`loop`) e l'interazione con l'hardware.
* **`alarm.c`** — Modulo C contenente la logica di controllo e le funzioni di gestione dell'allarme.

## Introduzione

Si propone un progetto che simula un sistema di allarme domestico. Tutta la logica è gestita in un ambiente multithread; mentre sensori e attuatori sono simulati da componenti Arduino. In particolare, un sensore ad ultrasuoni simula la telecamera, un buzzer simula la sirena, due led simulano le luci della casa e un pulsante serve ad intervenire sugli stati dell’allarme. 

Quando viene rilevato un movimento:

- il sistema entra in uno stato di allarme
- la sirena e le luci si attivano (per mettere in allarme l’intruso)
- ogni nuovo evento viene salvato in un file di log

Per la logica del progetto è stata utilizzata un’architettura multithread in ambiente Linux. 

Arduino si occupa dell'acquisizione degli eventi e del controllo degli attuatori. Linux esegue il programma C multithread. La comunicazione tra Arduino e Linux avviene attraverso la porta seriale.

## La macchina a stati

Il sistema è una macchina a stati così composta:

- stato SPENTO → sensori e attuatori disattivati
- stato ATTIVO SAFE → sensori attivi, attuatori spenti
- stato ATTIVO ALARM → sensori attivi, attuatori attivi
- stato ATTIVO SILENCE → sensori attivi, sirena spenta, luci accese

<img width="1255" height="802" alt="image" src="https://github.com/user-attachments/assets/8a642898-7914-47ac-9733-bc0a1316a643" />


Gli eventi (come il rilevamento di un movimento o il click sul bottone) generano un cambio di stato. 

## Architettura software multithread

- T1 – Serial Thread: acquisizione degli eventi da Arduino.
- T2 – Alarm Thread: gestione degli eventi e aggiornamento dello stato.
- T3 – Actuator Thread: controllo periodico degli attuatori.
- T4 – Log Thread: registrazione degli eventi e dei tempi di attivazione in un file di log

<img width="773" height="386" alt="image" src="https://github.com/user-attachments/assets/6f411582-35f0-4adf-b1ae-5f04514b30ba" />


## Strutture condivise, sincronizzazione

Nel codice vengono utilizzate principalmente due strutture condivise: **EventQueue** e **AlarmSystem**. A queste si aggiunge il **mutex** dedicato alla scrittura del file di log.

#### 1. Coda degli eventi (EventQueue)

<img width="376" height="127" alt="image" src="https://github.com/user-attachments/assets/5bf20343-6bb5-4c7c-90ad-b75222b26386" />

<img width="487" height="223" alt="image" src="https://github.com/user-attachments/assets/b0a5a586-ecf8-43da-a097-8197bfdce1bd" />

La prima struttura condivisa è `EventQueue`, che permette la comunicazione tra T1 (Serial Thread) e T2 (Alarm Thread). T1 si occupa di leggere i messaggi provenienti da Arduino attraverso la porta seriale. Quando riceve un messaggio valido, ad esempio `MOTION,1` oppure `BUTTON,PRESS`, lo converte in un evento interno di tipo `Event`, contenente due informazioni:

- `type`: identifica il tipo di evento, ad esempio rilevamento di movimento, cessazione del movimento o pressione del pulsante.
- `timestamp`: registra il momento in cui l'evento è stato rilevato, utilizzando `CLOCK_MONOTONIC`.

Una volta creato l'evento, T1 lo inserisce nella coda attraverso la funzione `queue_push()`.

La coda è implementata come un buffer circolare di dimensione fissa (32 eventi). Le variabili `head`, `tail` e `count` permettono rispettivamente di individuare la posizione del prossimo elemento da estrarre, la posizione in cui inserire il prossimo elemento e il numero di eventi attualmente presenti.

T2, invece, utilizza la funzione `queue_pop()` per estrarre gli eventi dalla coda e processarli. In questo modo, T1 e T2 lavorano in maniera indipendente.

La coda utilizza due meccanismi di sincronizzazione:

- Mutex (`q->mutex`): protegge l'accesso alla coda. Sia `queue_push()` sia `queue_pop()` acquisiscono il mutex prima di modificare o leggere gli elementi e lo rilasciano al termine dell'operazione.
- Condition variable (`q->not_empty`): permette a T2 di attendere quando la coda è vuota, senza dover controllare continuamente se sono arrivati nuovi eventi. Quando T1 inserisce un evento, utilizza `pthread_cond_signal()` per segnalare che la coda non è più vuota e risvegliare il thread in attesa.

La funzione `pthread_cond_wait()` rilascia temporaneamente il mutex mentre T2 attende e lo riacquisisce quando viene risvegliato.

Se la coda raggiunge la capacità massima di 32 eventi, `queue_push()` rifiuta il nuovo evento e restituisce `false`, evitando di sovrascrivere eventi già presenti.

#### 2. Struttura condivisa AlarmSystem

<img width="406" height="170" alt="image" src="https://github.com/user-attachments/assets/b689f5a6-256f-4167-b242-9e29afc0cee5" />
<img width="458" height="150" alt="image" src="https://github.com/user-attachments/assets/d1c3e08b-9230-4508-ab4b-78ffd1bb1364" />

<img width="427" height="178" alt="image" src="https://github.com/user-attachments/assets/471553e2-eb0f-49c5-9c8c-8425c4572f22" />


La seconda struttura condivisa è `AlarmSystem`, che contiene le informazioni sullo stato corrente del sistema e sugli attuatori. È composta da:

- `alarm_state`: rappresenta lo stato attuale dell'allarme.
- `actuators`: è una struttura di tipo `ActuatorState` che contiene tre variabili booleane:
    - `sensor_enabled`: indica se il sensore è attivo;
    - `lights_on`: indica se le luci sono accese;
    - `siren_on`: indica se la sirena è attiva.
- `mutex`: protegge l'accesso alle informazioni condivise.

La struttura viene inizializzata dal main e successivamente condivisa tra T2, T3 e T4.

Alarm Thread (T2) è il thread responsabile dell'elaborazione degli eventi e dell'aggiornamento dello stato del sistema. Quando estrae un evento dalla coda, richiama `alarm_system_process_event()`, che acquisisce il mutex di `AlarmSystem`, legge lo stato precedente, calcola il nuovo stato attraverso `update_alarm_state()` e verifica se è avvenuto un cambiamento. Se lo stato è cambiato, aggiorna anche le azioni richieste sugli attuatori attraverso `get_actuator_state()`. Infine, rilascia il mutex. Se è avvenuto un cambiamento, T2 stampa il nuovo stato sul monitor e comunica ad Arduino le azioni da eseguire attraverso `apply_actuators()`.

#### 3. Accesso concorrente ad AlarmSystem: snapshot

Oltre alla modifica dello stato da parte di T2, è importante considerare che anche T3 e T4 devono poter leggere le informazioni contenute in `AlarmSystem`.

- T2 – Alarm Thread modifica lo stato dell'allarme e i valori degli attuatori.
- T3 – Monitor Thread legge periodicamente lo stato del sistema e degli attuatori per monitorarne il funzionamento.
- T4 – Logger Thread legge periodicamente le stesse informazioni per individuare eventuali cambiamenti e registrarli nel file di log.

Per evitare che T3 o T4 leggano i dati proprio mentre T2 li sta modificando, viene utilizzata la funzione `alarm_system_get_snapshot()`. Questa funzione acquisisce il mutex di `AlarmSystem`, copia i valori di `alarm_state` e `actuators` in due variabili locali e infine rilascia il mutex. In questo modo, T3 e T4 ricevono una fotografia coerente dello stato del sistema in un determinato momento, senza poter osservare una situazione intermedia durante l'aggiornamento.

È importante sottolineare che il mutex non viene mantenuto durante tutte le operazioni di monitoraggio o logging: viene acquisito soltanto per il tempo necessario a copiare i dati condivisi. Questo riduce il tempo in cui gli altri thread devono attendere.

#### 4. Gestione della terminazione dei thread

Infine, è presente una variabile globale condivisa:

```
volatile sig_atomic_t stop_requested = 0;
```

Questa variabile viene utilizzata per comunicare ai thread la richiesta di terminazione del programma.

Quando l'utente preme CTRL+C, il gestore del segnale `handle_sigint()` imposta `stop_requested` a 1. I thread controllano periodicamente questa variabile per capire se devono terminare.

C'è anche un aspetto importante relativo alla coda: T2 potrebbe trovarsi in attesa all'interno di `pthread_cond_wait()` perché non ci sono eventi da elaborare. Per evitare che rimanga bloccato durante la terminazione, il main utilizza `pthread_cond_broadcast()` per risvegliarlo. In questo modo, T2 può verificare che è stata richiesta la terminazione e uscire dal proprio ciclo.

Il sistema utilizza quindi tre mutex distinti, ciascuno con una responsabilità specifica:

1. `queue.mutex`: protegge la coda degli eventi durante le operazioni di inserimento ed estrazione.
2. `system.mutex`: protegge lo stato dell'allarme e le informazioni relative agli attuatori.
3. `log_mutex`: impedisce che scritture concorrenti interferiscano con il file di log.

A questi si aggiunge una condition variable, che permette di sincronizzare T1 e T2 evitando che T2 rimanga continuamente in esecuzione quando non ci sono eventi da elaborare.

L'obiettivo complessivo è garantire che i thread possano lavorare in parallelo, mantenendo la coerenza delle informazioni condivise e coordinando correttamente le operazioni.

Presentazione: https://www.canva.com/design/DAHbQ3m9Kdw/4Ik19tkqq6-z5o7QVIqyAA/edit?ui=eyJBIjp7fX0

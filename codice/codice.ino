const int TRIG_PIN = 12;
const int ECHO_PIN = 13;
const int BUTTON_PIN = 3;
const int LED_RED_PIN = 8;
const int LED_BLUE_PIN = 7;
const int BUZZER_PIN = 5;

const float MOTION_THRESHOLD_CM = 50.0;
const unsigned long SENSOR_INTERVAL_MS = 100;
const unsigned long DEBOUNCE_MS = 40;

bool motionDetected = false;
bool lightsOn = false;
bool sirenOn = false;

// Stato del pulsante e debounce
bool lastRawButtonState = LOW;
bool stableButtonState = LOW;
unsigned long lastButtonChangeMs = 0;

// Temporizzazione del sensore
unsigned long lastSensorReadMs = 0;

// Buffer per i comandi ricevuti da Linux
char commandBuffer[64];
size_t commandLength = 0;


float readDistanceCm() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);

    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    unsigned long duration = pulseIn(ECHO_PIN, HIGH, 25000UL);

    if (duration == 0) {
        return -1.0;  // Nessuna misura valida
    }

    return duration * 0.0343 / 2.0;
}


void setLights(bool on) {
    lightsOn = on;
    digitalWrite(LED_RED_PIN, on ? HIGH : LOW);
    digitalWrite(LED_BLUE_PIN, on ? HIGH : LOW);
}


void setSiren(bool on) {
    sirenOn = on;

    if (on) {
        tone(BUZZER_PIN, 2000);
    } else {
        noTone(BUZZER_PIN);
    }
}


void processCommand(char *command) {
    if (strcmp(command, "LIGHTS,1") == 0) {
        setLights(true);
    } else if (strcmp(command, "LIGHTS,0") == 0) {
        setLights(false);
    } else if (strcmp(command, "SIREN,1") == 0) {
        setSiren(true);
    } else if (strcmp(command, "SIREN,0") == 0) {
        setSiren(false);
    }
}


void readSerialCommands() {
    while (Serial.available() > 0) {
        char c = Serial.read();

        if (c == '\n') {
            commandBuffer[commandLength] = '\0';

            // Rimuove l'eventuale CR finale
            if (commandLength > 0 &&
                commandBuffer[commandLength - 1] == '\r') {
                commandBuffer[commandLength - 1] = '\0';
            }

            processCommand(commandBuffer);
            commandLength = 0;
        } else if (commandLength < sizeof(commandBuffer) - 1) {
            commandBuffer[commandLength++] = c;
        } else {
            // Comando troppo lungo: scarta il buffer
            commandLength = 0;
        }
    }
}


void readMotionSensor() {
    unsigned long now = millis();

    if (now - lastSensorReadMs < SENSOR_INTERVAL_MS) {
        return;
    }

    lastSensorReadMs = now;

    float distance = readDistanceCm();

    // Una misura non valida non viene interpretata come assenza
    // di movimento: manteniamo lo stato precedente.
    if (distance < 0) {
        return;
    }

    bool newMotionState = distance <= MOTION_THRESHOLD_CM;

    if (newMotionState != motionDetected) {
        motionDetected = newMotionState;

        Serial.println(motionDetected ? "MOTION,1" : "MOTION,0");
    }
}


void readButton() {
    bool rawState = digitalRead(BUTTON_PIN);
    unsigned long now = millis();

    if (rawState != lastRawButtonState) {
        lastRawButtonState = rawState;
        lastButtonChangeMs = now;
    }

    if (now - lastButtonChangeMs >= DEBOUNCE_MS &&
        rawState != stableButtonState) {

        stableButtonState = rawState;

        // Comunica soltanto il fronte di pressione
        if (stableButtonState == HIGH) {
            Serial.println("BUTTON,PRESS");
        }
    }
}


void setup() {
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(BUTTON_PIN, INPUT);

    pinMode(LED_RED_PIN, OUTPUT);
    pinMode(LED_BLUE_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);

    setLights(false);
    setSiren(false);

    Serial.begin(9600);
    Serial.println("MOTION,0");
}


void loop() {
    readSerialCommands();
    readMotionSensor();
    readButton();
}

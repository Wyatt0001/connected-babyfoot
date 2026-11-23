#include <Arduino.h>
#include <WiFi.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>

const char*  WIFI_SSID = "Bbf2026";
const char*  WIFI_PASSWORD = "btscielir2";
const char*  QT_SERVER_IP = "192.168.1.150";
const uint16_t QT_PORT = 1234;

#define TRIG_PIN 27
#define ECHO_PIN 14

static const int PIN_RX2 = 16;
static const int PIN_TX2 = 17;

const float DISTANCE_SEUIL_CM  = 8.0;
const uint32_t COOLDOWN_MS = 10000;
const uint32_t WIFI_CHECK_MS = 5000;
const uint32_t TCP_TIMEOUT_MS     = 3000;
const uint8_t  MAX_PENDING_SCORES = 50;

static int      pendingScores[MAX_PENDING_SCORES];
static uint8_t  pendingHead  = 0;
static uint8_t  pendingTail  = 0;
static uint8_t  pendingCount = 0;
static SemaphoreHandle_t pendingMutex = nullptr;

uint32_t dernierBut  = 0;
int      scoreRouge  = 0;
volatile bool wifiConnected = false;

HardwareSerial  dfSerial(2);
DFRobotDFPlayerMini player;

float mesurerDistance();
bool  envoyerScore(int score);
bool  viderFilePending();
void  ajouterButPending(int score);
void  connecterWiFi();
void  tacheWiFi(void* param);
void  printDetail(uint8_t type, int value);

void setup()
{
    Serial.begin(115200);

    pendingMutex = xSemaphoreCreateMutex();

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    dfSerial.begin(9600, SERIAL_8N1, PIN_RX2, PIN_TX2);
    delay(1000);
    Serial.println("[DFPlayer] Initialisation...");
    if (!player.begin(dfSerial)) {
        Serial.println("[DFPlayer] ERREUR : non détecté. Vérifiez le câblage et la microSD (FAT32).");
    } else {
        Serial.println("[DFPlayer] Prêt.");
        player.volume(10);
    }
    connecterWiFi();

    xTaskCreatePinnedToCore(
        tacheWiFi,
        "WiFi_Task",
        4096,
        nullptr,
        1,
        nullptr,
        0
    );

    Serial.println("[INFO] ESP32 #1 — Équipe ROUGE prêt.");
}

void loop()
{
    if (player.available()) {
        printDetail(player.readType(), player.read());
    }

    float dist = mesurerDistance();
    Serial.printf("[Rouge] Distance : %.2f cm\n", dist);

    if (dist > 0 && dist < DISTANCE_SEUIL_CM)
    {
        uint32_t maintenant = millis();
        if (maintenant - dernierBut > COOLDOWN_MS)
        {
            dernierBut = maintenant;
            scoreRouge++;
            Serial.printf(">>> BUT ! Score Rouge : %d\n", scoreRouge);

            player.play(1);

            if (wifiConnected)
            {
                if (!envoyerScore(scoreRouge)) {
                    Serial.println("[TCP] Échec envoi → mise en attente.");
                    ajouterButPending(scoreRouge);
                }
            }
            else
            {
                Serial.println("[WiFi] Hors ligne → but mis en attente.");
                ajouterButPending(scoreRouge);
            }
        }
    }

    delay(200);
}

void tacheWiFi(void* param)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(WIFI_CHECK_MS));

        if (WiFi.status() != WL_CONNECTED)
        {
            wifiConnected = false;
            Serial.println("[WiFi] Connexion perdue — tentative de reconnexion…");
            WiFi.disconnect();
            WiFi.reconnect();

            uint8_t tentatives = 0;
            while (WiFi.status() != WL_CONNECTED && tentatives < 40) {
                vTaskDelay(pdMS_TO_TICKS(500));
                tentatives++;
            }

            if (WiFi.status() == WL_CONNECTED) {
                wifiConnected = true;
                Serial.printf("[WiFi] Reconnecté ! IP : %s\n",
                              WiFi.localIP().toString().c_str());
                viderFilePending();
            } else {
                Serial.println("[WiFi] Reconnexion échouée, nouvelle tentative dans 5 s.");
            }
        }
        else
        {
            wifiConnected = true;
            if (pendingCount > 0) {
                viderFilePending();
            }
        }
    }
}

void ajouterButPending(int score)
{
    if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (pendingCount < MAX_PENDING_SCORES) {
            pendingScores[pendingTail] = score;
            pendingTail = (pendingTail + 1) % MAX_PENDING_SCORES;
            pendingCount++;
            Serial.printf("[Pending] But %d ajouté (%d en attente).\n",
                          score, pendingCount);
        } else {
            Serial.println("[Pending] AVERTISSEMENT : file pleine, but perdu !");
        }
        xSemaphoreGive(pendingMutex);
    }
}

bool viderFilePending()
{
    if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;

    if (pendingCount == 0) {
        xSemaphoreGive(pendingMutex);
        return true;
    }

    Serial.printf("[Pending] Envoi de %d but(s) en attente…\n", pendingCount);

    while (pendingCount > 0)
    {
        int score = pendingScores[pendingHead];
        xSemaphoreGive(pendingMutex);

        if (envoyerScore(score))
        {
            if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                pendingHead = (pendingHead + 1) % MAX_PENDING_SCORES;
                pendingCount--;
                xSemaphoreGive(pendingMutex);
            }
        }
        else
        {
            Serial.println("[Pending] Échec TCP — envoi différé à la prochaine reconnexion.");
            return false;
        }

        if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    }

    xSemaphoreGive(pendingMutex);
    Serial.println("[Pending] File vidée avec succès.");
    return true;
}

bool envoyerScore(int score)
{
    WiFiClient envoi;
    envoi.setTimeout(TCP_TIMEOUT_MS);

    if (!envoi.connect(QT_SERVER_IP, QT_PORT)) {
        Serial.println("[TCP] Impossible de joindre le serveur Qt !");
        return false;
    }

    // Format : "R:<score>"  ex: "R:3"
    String message = "R" + String(score);
    envoi.print(message);
    envoi.flush();
    envoi.stop();

    Serial.printf("[TCP] '%s' envoyé avec succès.\n", message.c_str());
    return true;
}

void connecterWiFi()
{
    Serial.printf("[WiFi] Connexion à %s …", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint8_t tentatives = 0;
    while (WiFi.status() != WL_CONNECTED && tentatives < 40) {
        delay(500);
        Serial.print(".");
        tentatives++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.printf("\n[WiFi] Connecté ! IP : %s\n",
                      WiFi.localIP().toString().c_str());
    } else {
        wifiConnected = false;
        Serial.println("\n[WiFi] ÉCHEC — le programme continue en mode hors ligne.");
    }
}

float mesurerDistance()
{
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duree = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duree == 0) return -1.0;
    return (duree * 0.034f) / 2.0f;
}

void printDetail(uint8_t type, int value)
{
    switch (type) {
        case TimeOut:            Serial.println("[DFPlayer] Timeout série.");       break;
        case WrongStack:         Serial.println("[DFPlayer] Erreur de pile.");      break;
        case DFPlayerCardInserted: Serial.println("[DFPlayer] Carte SD insérée."); break;
        case DFPlayerCardRemoved:  Serial.println("[DFPlayer] Carte SD retirée."); break;
        case DFPlayerCardOnline:   Serial.println("[DFPlayer] Carte SD en ligne."); break;
        case DFPlayerPlayFinished:
            Serial.printf("[DFPlayer] Piste %d terminée.\n", value);
            break;
        case DFPlayerError:
            Serial.print("[DFPlayer] Erreur : ");
            switch (value) {
                case Busy:             Serial.println("occupé.");              break;
                case Sleeping:         Serial.println("en veille.");           break;
                case SerialWrongStack: Serial.println("erreur série.");        break;
                case CheckSumNotMatch: Serial.println("checksum invalide.");   break;
                case FileIndexOut:     Serial.println("index hors limites.");  break;
                case FileMismatch:     Serial.println("fichier introuvable."); break;
                case Advertise:        Serial.println("mode publicité.");      break;
                default:               Serial.println("inconnue.");            break;
            }
            break;
        default: break;
    }
}
#include <Arduino.h>
#include <WiFi.h>

// --- CONFIGURATION ---
const char* ssid = "Bbf2026";
const char* password = "btscielir2";

// REMPLACE PAR L'IP DU PC DE TON AMI
const char* serverIp = "192.168.1.119"; 
const uint16_t port = 8000;

void setup() {
    Serial.begin(115200);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n✅ Connecté au Wi-Fi !");
}

void loop() {
    WiFiClient client;
    
    // Tentative de connexion au PC de l'autre personne
    if (client.connect(serverIp, port)) {
        client.print("Message pour le PC distant (Local)");
        
        // Lire la réponse si le PC en envoie une
        while (client.available()) {
            String line = client.readStringUntil('\r');
            Serial.println("Réponse du PC : " + line);
        }
        client.stop();
    } else {
        Serial.println("❌ Échec : Vérifie l'IP ou le Pare-feu du PC cible");
    }
    delay(5000);
}
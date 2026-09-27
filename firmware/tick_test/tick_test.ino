// tick_test.ino — Diagnóstico mínimo ESP32-C3
// Sin Wire, sin TCA9535, sin shift register.
// Solo imprime un contador cada segundo + hace eco de lo que recibe.
// Si funciona → el canal serial está bien, el problema es en bring_up.ino
// Si NO funciona → problema en serial/USB o hardware del módulo

void setup() {
    Serial.begin(115200);
    delay(1500);  // esperar a que el USB-Serial/JTAG esté listo
    Serial.println("=== tick_test v1 ===");
    Serial.println("Escribe cualquier cosa y te hago eco.");
}

void loop() {
    static uint32_t last = 0;
    static uint32_t tick = 0;

    if (millis() - last >= 1000) {
        last = millis();
        Serial.printf("tick %lu\n", tick++);
    }

    if (Serial.available()) {
        String s = Serial.readStringUntil('\n');
        s.trim();
        if (s.length()) {
            Serial.printf("echo: %s\n", s.c_str());
        }
    }
}

#pragma once
#include <Arduino.h>

// Sur les cibles où le commissioning BLE est compilé (C3, S3, C6), Matter reçoit
// les identifiants Wi-Fi du contrôleur pendant l'appairage : tout ceci est inerte.
//
// Sur l'ESP32 classique, `CONFIG_ENABLE_CHIPOBLE` n'est pas activé dans les
// bibliothèques précompilées d'Arduino : il faut connecter le Wi-Fi soi-même
// avant de démarrer Matter, qui se met alors en service sur IP. Les identifiants
// sont stockés en NVS plutôt que codés en dur, pour éviter de recompiler.
void netBegin();
void netSetCredentials(const char *ssid, const char *password);
void netPrintStatus(Print &out);
bool netNeedsCredentials();

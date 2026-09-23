#include "net.h"

#include <sdkconfig.h>

#if !CONFIG_ENABLE_CHIPOBLE

#include <Preferences.h>
#include <WiFi.h>

static String ssid_;
static String pass_;

static void loadCredentials() {
  Preferences p;
  p.begin("benqhalo", true);
  ssid_ = p.getString("ssid", "");
  pass_ = p.getString("pass", "");
  p.end();
}

bool netNeedsCredentials() {
  if (ssid_.isEmpty()) loadCredentials();
  return ssid_.isEmpty();
}

void netBegin() {
  loadCredentials();
  if (ssid_.isEmpty()) {
    Serial.println();
    Serial.println("!! Aucun identifiant Wi-Fi enregistre.");
    Serial.println("!! Cette cible n'a pas le commissioning BLE : Matter a besoin du reseau.");
    Serial.println("!! Utilise : wifi <ssid> <mot-de-passe>");
    return;
  }

  Serial.printf("Wi-Fi : connexion a \"%s\"", ssid_.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_.c_str(), pass_.c_str());

  uint32_t deadline = millis() + 20000;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - deadline) < 0) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connecte, IP %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("!! Echec de connexion Wi-Fi. Verifie avec 'wifi <ssid> <mdp>'.");
  }
}

void netSetCredentials(const char *ssid, const char *password) {
  Preferences p;
  p.begin("benqhalo", false);
  p.putString("ssid", ssid);
  p.putString("pass", password);
  p.end();
  Serial.printf("Identifiants enregistres pour \"%s\". Redemarre avec 'reboot'.\n", ssid);
}

void netPrintStatus(Print &out) {
  out.printf("  Wi-Fi           : %s", WiFi.status() == WL_CONNECTED ? "connecte" : "non connecte");
  if (WiFi.status() == WL_CONNECTED) out.printf(" (%s)", WiFi.localIP().toString().c_str());
  out.println();
}

#else  // CONFIG_ENABLE_CHIPOBLE

void netBegin() {}
void netSetCredentials(const char *, const char *) {
  Serial.println("Inutile ici : Matter recupere le Wi-Fi du controleur pendant l'appairage BLE.");
}
#if MATTER_NET_THREAD
void netPrintStatus(Print &out) { out.println("  reseau          : Thread (Apple Home donne le dataset pendant l'appairage BLE)"); }
#else
void netPrintStatus(Print &out) { out.println("  Wi-Fi           : fourni par le commissioning Matter (BLE)"); }
#endif
bool netNeedsCredentials() { return false; }

#endif

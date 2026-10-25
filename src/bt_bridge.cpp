#include "bt_bridge.h"
#include "config.h"
#include "mount.h"
#include "message.h"
#include "xlate.h"
#include "BluetoothSerial.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include <Preferences.h>
#include <string.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Classic Bluetooth is not enabled (use the default esp32dev Bluedroid build)."
#endif

static BluetoothSerial Bt;
volatile bool btConnected = false;

static const int MAX_BONDS = 16;

// state shared with the Bluedroid task (callbacks) - plain flags, consumed in btTick()
static volatile bool     authDone = false, authOk = false;
static esp_bd_addr_t     authBda;
static char              authName[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static volatile uint32_t clientHandle = 0;
static volatile uint32_t bondSerial = 0;
static int               bondsSeen = 0;          // bond count at the last auth: a rise = new pairing
static volatile bool     openPending = false;   // SRV_OPEN seen, handled in btTick()
static volatile uint32_t openHandle = 0;
static esp_bd_addr_t     openBda;

static void setVisible() {
  esp_err_t e = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
  LOG("[bt] visible -> %s\n", esp_err_to_name(e));
}

static void macKey(const uint8_t *m, char *k) {          // NVS key: 12 hex chars
  snprintf(k, 13, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// number of bonds copied into `list`, or -1 if the stack would not tell us
static int bondList(esp_bd_addr_t *list) {
  int n = MAX_BONDS;
  if (esp_bt_gap_get_bond_device_num() <= 0) return 0;
  if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) return -1;
  return n;
}

static void logMac(const char *what, const uint8_t *m) {
  LOG("[bt] %s %02X:%02X:%02X:%02X:%02X:%02X\n", what, m[0], m[1], m[2], m[3], m[4], m[5]);
}

// ---- Bluedroid callbacks (BT task) ---------------------------------------------
static void sppEvent(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  switch (event) {
    case ESP_SPP_INIT_EVT:
      setVisible();
      break;
    case ESP_SPP_SRV_OPEN_EVT:
      if (param->srv_open.status != ESP_SPP_SUCCESS || btConnected || openPending) break;
      memcpy(openBda, param->srv_open.rem_bda, ESP_BD_ADDR_LEN);
      openHandle = param->srv_open.handle;
      openPending = true;
      break;
    case ESP_SPP_CLOSE_EVT:
      if (openPending && param->close.handle == openHandle) openPending = false;
      if (btConnected && param->close.handle == clientHandle) {
        btConnected = false;
        clientHandle = 0;
        LOG("[bt] closed\n");
      }
      break;
    default: break;
  }
}

// Replaces the library's GAP handler (only needed for master-mode discovery).
static void gapEvent(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_CFM_REQ_EVT:                 // Just Works: nothing to compare - accept
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      LOG("[bt] gap: pairing request accepted\n");
      break;
    case ESP_BT_GAP_KEY_REQ_EVT:                 // we have no keypad
      esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
      LOG("[bt] gap: passkey request rejected\n");
      break;
    case ESP_BT_GAP_PIN_REQ_EVT: {               // legacy (pre-2.1) peers
      esp_bt_pin_code_t pin;
#ifdef BT_PIN
      const char *p = BT_PIN;
#else
      const char *p = "1234";
#endif
      uint8_t len = strlen(p);
      memcpy(pin, p, len);
      esp_bt_gap_pin_reply(param->pin_req.bda, true, len, pin);
      LOG("[bt] gap: legacy PIN request answered\n");
      break;
    }
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      memcpy(authBda, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
      strncpy(authName, (const char *)param->auth_cmpl.device_name, sizeof authName - 1);
      authName[sizeof authName - 1] = 0;
      authOk = param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS;
      authDone = true;
      LOG("[bt] gap: auth complete '%s' status %d\n", authName, (int)param->auth_cmpl.stat);
      break;
    default: break;
  }
}

// ---- public -----------------------------------------------------------------
void btBegin() {
  Bt.register_callback(sppEvent);
  uint32_t heap = ESP.getFreeHeap();
  if (!Bt.begin(BT_NAME)) { LOG("[bt] begin FAILED (free heap %u)\n", (unsigned)heap); return; }
  esp_bt_gap_register_callback(gapEvent);
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;    // no display, no keys -> "Just Works"
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof iocap);
  setVisible();
  bondsSeen = btBondCount();
  LOG("[bt] '%s' up, open (Just Works), %d paired (free heap %u -> %u)\n", BT_NAME, bondsSeen,
      (unsigned)heap, (unsigned)ESP.getFreeHeap());
}

void btTick() {
  if (openPending) {
    openPending = false;
    clientHandle = openHandle;
    mountDrainRx();                 // start the session clean
    xlateReset();
    btConnected = true;
    logMac("connected", openBda);
    LOG("[bt] client protocol: %s\n", xlateModeName(xlateMode()));
  }
  if (authDone) {
    authDone = false;
    char k[13]; macKey(authBda, k);
    if (authOk) {
      Preferences p;
      if (p.begin("btnames", false)) { p.putString(k, authName[0] ? authName : ""); p.end(); }
      int n = btBondCount();
      if (n > bondsSeen) {          // a new bond, not a reconnect with a stored key
        LOG("[bt] paired '%s' (%s)\n", authName, k);
        char m[40]; snprintf(m, sizeof m, "Paired: %.24s", authName[0] ? authName : k);
        setMsg(m, C_OK, 4000);
      }
      bondsSeen = n;
    } else {
      LOG("[bt] pairing with %s failed\n", k);
    }
    bondSerial++;
  }
}

void btBridgeTick() {
  if (xlateMode() == XL_NATIVE) {
    pumpClientToMount(Bt);          // client -> mount
    pumpMountToClient(Bt);          // mount  -> client
    return;
  }
  // LX200 / NexStar: every client byte goes through the translator, which does
  // its own request/reply with the mount (bounded by available(): never blocks
  // on the client)
  for (int n = Bt.available(); n > 0; n--) {
    int c = Bt.read();
    if (c < 0) break;
    xlateFeed((uint8_t)c);
  }
  mountDrainRx();                   // nothing unsolicited may reach a translated client
  xlateTick();
  static uint32_t pingMs = 0;       // idle client: keep the mount-link LED honest
  if (millis() - pingMs > 2000) {
    pingMs = millis();
    char r[16];
    askMount(":GAS#", r, sizeof r, 300);
  }
}

void btWrite(const uint8_t *buf, size_t n) {
  if (btConnected) Bt.write(buf, n);
}

int btBondCount() {
  int n = esp_bt_gap_get_bond_device_num();
  return n < 0 ? 0 : n;
}

bool btBondGet(int i, uint8_t mac[6], char *name, size_t n) {
  esp_bd_addr_t list[MAX_BONDS];
  int cnt = bondList(list);
  if (i < 0 || i >= cnt) return false;
  memcpy(mac, list[i], 6);
  char k[13]; macKey(mac, k);
  name[0] = 0;
  Preferences p;
  if (p.begin("btnames", true)) { p.getString(k, name, n); p.end(); }
  if (!name[0])
    snprintf(name, n, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return true;
}

static void forget(const uint8_t *mac) {
  char k[13]; macKey(mac, k);
  esp_bt_gap_remove_bond_device((uint8_t *)mac);   // also drops a live link to it
  Preferences p;
  if (p.begin("btnames", false)) { p.remove(k); p.end(); }
  LOG("[bt] forgot %s\n", k);
}

bool btBondForget(int i) {
  uint8_t mac[6]; char nm[4];
  if (!btBondGet(i, mac, nm, sizeof nm)) return false;
  forget(mac);
  bondsSeen = btBondCount();
  bondSerial++;
  return true;
}

void btBondForgetAll() {
  esp_bd_addr_t list[MAX_BONDS];
  int cnt = bondList(list);
  for (int i = 0; i < cnt; i++) forget(list[i]);
  Preferences p;
  if (p.begin("btnames", false)) { p.clear(); p.end(); }
  bondsSeen = 0;
  bondSerial++;
}

uint32_t btBondSerial() { return bondSerial; }

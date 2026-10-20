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
static volatile bool     pairOpen = false;
static volatile uint32_t pairUntil = 0;
static volatile bool     cfmPending = false;
static volatile uint32_t cfmCode = 0;
static esp_bd_addr_t     cfmBda;
static volatile bool     authDone = false, authOk = false;
static esp_bd_addr_t     authBda;
static char              authName[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static volatile uint32_t clientHandle = 0;
static volatile uint32_t bondSerial = 0;
static bool              codeAccepted = false;  // main task: BOOT accepted the code of...
static esp_bd_addr_t     acceptedBda;           // ...this peer; its auth success = new pairing
static volatile bool     openPending = false;   // SRV_OPEN seen, bond check due in btTick()
static volatile uint32_t openHandle = 0;
static esp_bd_addr_t     openBda;

static void setDiscoverable(bool on) {
  esp_err_t e = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                         on ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
  LOG("[bt] scan mode %s -> %s\n", on ? "discoverable" : "hidden", esp_err_to_name(e));
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

// -1 = unknown (lookup failed), 0 = not bonded, 1 = bonded
static int bondState(const uint8_t *bda) {
  esp_bd_addr_t list[MAX_BONDS];
  int n = bondList(list);
  if (n < 0) return -1;
  for (int i = 0; i < n; i++) if (!memcmp(list[i], bda, ESP_BD_ADDR_LEN)) return 1;
  return 0;
}

static void logMac(const char *what, const uint8_t *m) {
  LOG("[bt] %s %02X:%02X:%02X:%02X:%02X:%02X\n", what, m[0], m[1], m[2], m[3], m[4], m[5]);
}

// ---- Bluedroid callbacks (BT task) ---------------------------------------------
static void sppEvent(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  switch (event) {
    case ESP_SPP_INIT_EVT:
      setDiscoverable(pairOpen);    // the library just made us discoverable - undo
      break;
    case ESP_SPP_SRV_OPEN_EVT:
      // Only note it here: the bond lookup takes stack locks, so it runs in
      // btTick() on the main task. Bytes that arrive meanwhile stay buffered.
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
    case ESP_BT_GAP_CFM_REQ_EVT: {               // numeric comparison
      bool ask = pairOpen && !cfmPending;
      if (ask) {
        memcpy(cfmBda, param->cfm_req.bda, ESP_BD_ADDR_LEN);
        cfmCode = param->cfm_req.num_val;
        cfmPending = true;
      } else {
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, false);
      }
      LOG("[bt] gap: confirm request %06lu (%s)\n", (unsigned long)param->cfm_req.num_val,
          ask ? "asking user" : pairOpen ? "REJECTED - another code pending"
                                         : "REJECTED - pairing window closed");
      break;
    }
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
      esp_bt_gap_pin_reply(param->pin_req.bda, pairOpen, len, pin);
      LOG("[bt] gap: legacy PIN request (%s)\n", pairOpen ? "answered" : "rejected");
      break;
    }
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      memcpy(authBda, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
      strncpy(authName, (const char *)param->auth_cmpl.device_name, sizeof authName - 1);
      authName[sizeof authName - 1] = 0;
      authOk = param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS;
      // the attempt is over (accepted, rejected, or timed out on the host): a
      // code still waiting for BOOT is stale - drop it, or every retry in this
      // window is refused and BOOT would accept the old code
      if (cfmPending && !memcmp(cfmBda, param->auth_cmpl.bda, ESP_BD_ADDR_LEN)) cfmPending = false;
      authDone = true;
      LOG("[bt] gap: auth complete '%s' status %d\n", authName, (int)param->auth_cmpl.stat);
      break;
    default: break;
  }
}

// ---- public -----------------------------------------------------------------
void btBegin() {
  Bt.register_callback(sppEvent);
  Bt.enableSSP();                   // IO capability "display + yes/no" -> numeric comparison
  uint32_t heap = ESP.getFreeHeap();
  if (!Bt.begin(BT_NAME)) { LOG("[bt] begin FAILED (free heap %u)\n", (unsigned)heap); return; }
  esp_bt_gap_register_callback(gapEvent);
  setDiscoverable(false);
  LOG("[bt] '%s' up, hidden, %d paired (free heap %u -> %u)\n", BT_NAME, btBondCount(),
      (unsigned)heap, (unsigned)ESP.getFreeHeap());
}

void btTick() {
  if (openPending) {
    openPending = false;
    int b = bondState(openBda);
    if (b == 0) {                   // never paired with us: drop it
      logMac("rejected unpaired", openBda);
      esp_spp_disconnect(openHandle);
    } else {                        // paired - or lookup failed: don't lock out a real client
      if (b < 0) logMac("bond lookup failed, accepting", openBda);
      clientHandle = openHandle;
      mountDrainRx();               // start the session clean
      xlateReset();
      btConnected = true;
      logMac("connected", openBda);
      LOG("[bt] client protocol: %s\n", xlateModeName(xlateMode()));
    }
  }
  if (pairOpen && (int32_t)(millis() - pairUntil) >= 0) {
    btPairStop();
    setMsg("Pairing window closed", C_LBL, 3000);
  }
  if (authDone) {
    authDone = false;
    // A new pairing is an auth that completes after the user accepted its code.
    // Any other successful auth used a stored link key: Android does one of those
    // right before it starts the real pairing, so it must not close the window.
    bool viaCode = codeAccepted && !memcmp(acceptedBda, authBda, ESP_BD_ADDR_LEN);
    char k[13]; macKey(authBda, k);
    if (authOk) {
      Preferences p;
      if (p.begin("btnames", false)) { p.putString(k, authName[0] ? authName : ""); p.end(); }
      if (viaCode) {
        codeAccepted = false;
        LOG("[bt] paired '%s' (%s)\n", authName, k);
        char m[40]; snprintf(m, sizeof m, "Paired: %.24s", authName[0] ? authName : k);
        setMsg(m, C_OK, 4000);
        if (pairOpen) btPairStop(); // one device per window
      } else LOG("[bt] authenticated '%s' (%s) with a stored key\n", authName, k);
    } else {
      if (viaCode) codeAccepted = false;
      if (pairOpen) {
        LOG("[bt] pairing failed\n");
        setMsg("Pairing failed - try again", C_BAD, 4000);
      }
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

void btPairStart(uint32_t ms) {
  pairUntil = millis() + ms;
  codeAccepted = false;
  pairOpen = true;
  setDiscoverable(true);
  LOG("[bt] pairing window open %lus\n", (unsigned long)(ms / 1000));
}

void btPairStop() {
  if (cfmPending) { esp_bt_gap_ssp_confirm_reply(cfmBda, false); cfmPending = false; }
  pairOpen = false;
  setDiscoverable(false);
  LOG("[bt] hidden\n");
}

bool     btPairing()    { return pairOpen; }
uint32_t btPairLeftMs() { return pairOpen ? (uint32_t)max<int32_t>(0, pairUntil - millis()) : 0; }

bool btConfirmPending(uint32_t *code) {
  if (cfmPending && code) *code = cfmCode;
  return cfmPending;
}

void btConfirmReply(bool accept) {
  if (!cfmPending) return;
  if (accept) { memcpy(acceptedBda, cfmBda, ESP_BD_ADDR_LEN); codeAccepted = true; }   // before the reply
  esp_bt_gap_ssp_confirm_reply(cfmBda, accept);
  cfmPending = false;
  LOG("[bt] code %06lu %s\n", (unsigned long)cfmCode, accept ? "accepted" : "rejected");
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
  bondSerial++;
  return true;
}

void btBondForgetAll() {
  esp_bd_addr_t list[MAX_BONDS];
  int cnt = bondList(list);
  for (int i = 0; i < cnt; i++) forget(list[i]);
  Preferences p;
  if (p.begin("btnames", false)) { p.clear(); p.end(); }
  bondSerial++;
}

uint32_t btBondSerial() { return bondSerial; }

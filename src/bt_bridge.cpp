#include "bt_bridge.h"
#include "config.h"
#include "mount.h"
#include "BluetoothSerial.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Classic Bluetooth is not enabled (use the default esp32dev Bluedroid build)."
#endif

static BluetoothSerial Bt;
volatile bool btConnected = false;

static void btEvent(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  if (event == ESP_SPP_SRV_OPEN_EVT) {
    btConnected = true;
    mountDrainRx();                 // start the session clean
    LOG("[bt] connected\n");
  } else if (event == ESP_SPP_CLOSE_EVT) {
    btConnected = false;
    LOG("[bt] closed\n");
  }
}

void btBegin() {
  Bt.register_callback(btEvent);
  Bt.begin(BT_NAME);
#ifdef BT_PIN
  Bt.setPin(BT_PIN, strlen(BT_PIN));
#endif
}

void btBridgeTick() {
  pumpClientToMount(Bt);            // client -> mount
  pumpMountToClient(Bt);            // mount  -> client
}

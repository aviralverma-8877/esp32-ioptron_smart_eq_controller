#include "bt_bridge.h"
#include "config.h"
#include "mount.h"
#include "BluetoothSerial.h"
#include "esp_bt_device.h"

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
  } else {
    LOG("[bt] spp event %d\n", (int)event);   // TEMP: pairing debug
  }
}

void btBegin() {
  Bt.register_callback(btEvent);
  uint32_t heap = ESP.getFreeHeap();
  if (!Bt.begin(BT_NAME)) LOG("[bt] begin FAILED (free heap %u)\n", (unsigned)heap);
  else                    LOG("[bt] '%s' discoverable (free heap %u -> %u)\n",
                              BT_NAME, (unsigned)heap, (unsigned)ESP.getFreeHeap());
  const uint8_t *mac = esp_bt_dev_get_address();   // TEMP: pairing debug
  if (mac) LOG("[bt] addr %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#ifdef BT_PIN
  Bt.setPin(BT_PIN, strlen(BT_PIN));
#endif
}

void btBridgeTick() {
  pumpClientToMount(Bt);            // client -> mount
  pumpMountToClient(Bt);            // mount  -> client
}

#include <mgos.h>

#include <mgos_arduino_dallas_temp.h>
#include <mgos_homeassistant.h>

#include <mgos-helpers/log.h>
#include <mgos-helpers/mem.h>

struct ds18x_ha {
  DeviceAddress dev;
  struct mgos_homeassistant_object *o;
  float tempC;
  SLIST_ENTRY(ds18x_ha) entry;
};
static SLIST_HEAD(, ds18x_ha) cfg;

static void ha_dsh_temperature(struct mgos_homeassistant_object *o,
                               struct json_out *out) {
  float tempC = ((struct ds18x_ha *) o->user_data)->tempC;
  if (tempC == DEVICE_DISCONNECTED_C) return;
  json_printf(out, "%.4f", tempC);
}

static struct mgos_homeassistant_object *ha_obj_add(
    struct mgos_homeassistant *ha, const char *name, struct ds18x_ha *dsh) {
  struct mgos_homeassistant_object *o = mgos_homeassistant_object_add(
      ha, name, COMPONENT_SENSOR, NULL, NULL, dsh);
  if (!o) FNERR_RET(NULL, "failed to add HA object %s", name);
  if (mgos_homeassistant_object_class_add(
          o, "temperature",
          "\"unit_of_meas\":\"°C\",\"stat_cla\":\"measurement\"",
          ha_dsh_temperature))
    return o;
  FNERR("failed to add %s class to HA object %s", "temperature", name);
  mgos_homeassistant_object_remove(&o);
  return NULL;
}

static struct mgos_homeassistant_object *ha_obj_get(DeviceAddress dev) {
  struct ds18x_ha *dsh;
  SLIST_FOREACH(dsh, &cfg, entry) {
    if (!memcmp(dsh->dev, dev, sizeof(DeviceAddress))) return dsh->o;
  }
  return NULL;
}

#define HA_DS18X_FMT "{addr:%H,name:%Q}"
static bool ha_obj_fromjson(struct mgos_homeassistant *ha,
                            struct json_token v) {
  struct ds18x_ha *dsh = NULL;
  uint8_t *addr = NULL;
  char *name = NULL;
  int addrL, nameL;
  bool ret = false;
  if (json_scanf(v.ptr, v.len, HA_DS18X_FMT, &addrL, &addr, &name, &nameL) < 0)
    FNERR_GT("%s(%.*s): %s", "json_scanf", v.len, v.ptr, "failed");
  if (!addr) FNERR_GT("no %s in %.*s", "addr", v.len, v.ptr);
  if (addrL != sizeof(DeviceAddress))
    FNERR_GT("need %u byte %s, got %d: %.*s", sizeof(DeviceAddress), "addr",
             addrL, v.len, v.ptr);
  if (ha_obj_get(addr)) FNERR_GT("duplicate addr: %.*s", v.len, v.ptr);
  if (!name) FNERR_GT("no %s in %.*s", "name", v.len, v.ptr);

  dsh = (struct ds18x_ha *) TRY_MALLOC_OR(goto err, dsh);
  memcpy(dsh->dev, addr, sizeof(dsh->dev));
  dsh->tempC = DEVICE_DISCONNECTED_C;
  if (!(dsh->o = ha_obj_add(ha, name, dsh))) goto err;
  SLIST_INSERT_HEAD(&cfg, dsh, entry);
  ret = true;

err:
  if (addr) free(addr);
  if (name) free(name);
  if (!ret && dsh && dsh->o) mgos_homeassistant_object_remove(&dsh->o);
  if (!ret && dsh) free(dsh);
  return ret;
}

static void dsh_timer_data(void *opaque) {
  DallasTemperature *ds18x = mgos_ds18x_get_global_locked();
  for (uint8_t idx = 0; idx < ds18x->getDeviceCount(); idx++) {
    DeviceAddress dev;
    if (!ds18x->getAddress(dev, idx))
      FNERR_CONT("%s(%u): %s", "getAddress", idx, "not found");
    float c = ds18x->getTempC(dev);
    if (c == DEVICE_DISCONNECTED_C)
      FNERR_CONT("%s(%u): %s", "getTemp", idx, "disconnected");
    struct mgos_homeassistant_object *o = ha_obj_get(dev);
    if (!o) continue;
    ((struct ds18x_ha *) o->user_data)->tempC = c;
    mgos_homeassistant_object_send_status(o);
  }
  mgos_ds18x_put_global_locked();
}

static void dsh_timer_period(void *opaque) {
  DallasTemperature *ds18x = mgos_ds18x_get_global_locked();
  bool wfc = ds18x->getWaitForConversion();
  if (wfc) ds18x->setWaitForConversion(false);
  ds18x->requestTemperatures();
  if (wfc) ds18x->setWaitForConversion(true);
  uint16_t conv_ms = ds18x->millisToWaitForConversion();
  mgos_ds18x_put_global_locked();
  if (mgos_set_timer(conv_ms, 0, dsh_timer_data, NULL) == MGOS_INVALID_TIMER_ID)
    FNERR("mgos_set_timer(%u): %d", conv_ms, MGOS_INVALID_TIMER_ID);
}

extern "C" bool mgos_ds18x_ha_init(void) {
  if (!mgos_sys_config_get_ds18x_ha_enable()) return true;
  DallasTemperature *ds18x = mgos_ds18x_get_global_locked();
  if (!ds18x) return true;
  ds18x->setWaitForConversion(false);
  mgos_ds18x_put_global_locked();
  TRY_RETF(mgos_homeassistant_register_provider, "ds18x", ha_obj_fromjson,
           NULL);

  int ms = mgos_sys_config_get_ds18x_ha_period() * 1000;
  if (ms < 5000) ms = 5000;
  if (mgos_set_timer(ms, MGOS_TIMER_REPEAT | MGOS_TIMER_RUN_NOW,
                     dsh_timer_period, NULL) == MGOS_INVALID_TIMER_ID)
    FNERR_RET(false, "mgos_set_timer(%u): %d", ms, MGOS_INVALID_TIMER_ID);
  return true;
}

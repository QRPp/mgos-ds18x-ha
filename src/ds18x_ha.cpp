#include <mgos.h>

#include <mgos_arduino_dallas_temp.h>
#include <mgos_homeassistant.h>

#include <mgos-helpers/log.h>
#include <mgos-helpers/mem.h>

struct ds18x_ha {
  float tempC;
};

static char *ha_cfg() {
  const char *fn = mgos_sys_config_get_homeassistant_config();
  return !fn ? NULL : json_fread(fn);
}

#define HA_DS18X_FMT "{addr:%H,name:%Q}"
static char *ha_cfg_get_name_if_json_match(struct json_token *v,
                                           DeviceAddress dev) {
  char *addr = NULL, *name = NULL, *ret = NULL;
  int addrL, nameL;
  if (json_scanf(v->ptr, v->len, HA_DS18X_FMT, &addrL, &addr, &name, &nameL) <
      0)
    FNERR_GT("json_scanf(%.*s): %s", v->len, v->ptr, "failed");
  if (!addr) FNERR_GT("no %s in %.*s", "addr", v->len, v->ptr);
  if (addrL != sizeof(DeviceAddress))
    FNERR_GT("need %u byte %s, got %d: %.*s", sizeof(DeviceAddress), "addr",
             addrL, v->len, v->ptr);
  if (!name) FNERR_GT("no %s in %.*s", "name", v->len, v->ptr);
  if (!memcmp(addr, dev, sizeof(DeviceAddress))) ret = name;
err:
  if (addr) free(addr);
  if (name && !ret) free(name);
  return ret;
}

#define HA_CONF_PATH ".provider.ds18x"
static char *ha_cfg_get_name(DeviceAddress dev) {
  char *list = ha_cfg(), *name = NULL;
  if (!list) return NULL;
  size_t len = strlen(list);
  void *h = NULL;
  struct json_token v;
  while ((h = json_next_elem(list, len, h, HA_CONF_PATH, NULL, &v)) != NULL &&
         (name = ha_cfg_get_name_if_json_match(&v, dev)) == NULL)
    ;
  free(list);
  return name;
}

static void ha_dsh_temperature(struct mgos_homeassistant_object *o,
                               struct json_out *out) {
  json_printf(out, "%.4f", ((struct ds18x_ha *) o->user_data)->tempC);
}

static struct mgos_homeassistant_object *ha_obj_add(
    struct mgos_homeassistant *ha, const char *name) {
  struct mgos_homeassistant_object *o = NULL;
  struct ds18x_ha *dsh = NULL;
  dsh = (struct ds18x_ha *) TRY_MALLOC_OR(goto err, dsh);
  dsh->tempC = DEVICE_DISCONNECTED_C;

  o = mgos_homeassistant_object_add(ha, name, COMPONENT_SENSOR, NULL, NULL,
                                    dsh);
  if (!o) FNERR_GT("failed to add HA object %s", name);
  if (!mgos_homeassistant_object_class_add(
          o, "temperature",
          "\"unit_of_meas\":\"°C\",\"stat_cla\":\"measurement\"",
          ha_dsh_temperature))
    FNERR_GT("failed to add %s class to HA object %s", "temperature", name);

  FNLOG(LL_INFO, "added HA object %s", name);
  return o;

err:
  if (o) mgos_homeassistant_object_remove(&o);
  if (dsh) free(dsh);
  return NULL;
}

static char *ha_obj_get_name(char *buf, size_t sz, DeviceAddress dev) {
  char *name = ha_cfg_get_name(dev);
  const char *pfx = mgos_sys_config_get_ds18x_ha_name_prefix() ?: "";
  if (name) {
    snprintf(buf, sz, "%s%s", pfx, name);
    free(name);
  } else
    snprintf(buf, sz, "%s%08X%08X", pfx,
             dev[0] << 24 | dev[1] << 16 | dev[2] << 8 | dev[3],
             dev[4] << 24 | dev[5] << 16 | dev[6] << 8 | dev[7]);
  return buf;
}

static struct mgos_homeassistant_object *ha_obj_get_or_add(
    struct mgos_homeassistant *ha, DeviceAddress dev) {
  const char *name = ha_obj_get_name((char *) alloca(32), 32, dev);
  struct mgos_homeassistant_object *o = mgos_homeassistant_object_get(ha, name);
  return !o ? ha_obj_add(ha, name) : !strcmp(o->object_name, name) ? o : NULL;
}

static void dsh_timer_data(void *opaque) {
  struct mgos_homeassistant *ha = (struct mgos_homeassistant *) opaque;
  DallasTemperature *ds18x = mgos_ds18x_get_global_locked();
  for (uint8_t idx = 0; idx < ds18x->getDeviceCount(); idx++) {
    DeviceAddress dev;
    if (!ds18x->getAddress(dev, idx))
      FNERR_CONT("%s(%u): %s", "getAddress", idx, "not found");
    float c = ds18x->getTempC(dev);
    if (c == DEVICE_DISCONNECTED_C)
      FNERR_CONT("%s(%u): %s", "getTemp", idx, "disconnected");
    struct mgos_homeassistant_object *o = ha_obj_get_or_add(ha, dev);
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
  if (mgos_set_timer(conv_ms, 0, dsh_timer_data, opaque) ==
      MGOS_INVALID_TIMER_ID)
    FNERR("mgos_set_timer(%u): %d", conv_ms, MGOS_INVALID_TIMER_ID);
}

extern "C" bool mgos_ds18x_ha_init(void) {
  if (!mgos_sys_config_get_ds18x_ha_enable()) return true;
  DallasTemperature *ds18x = mgos_ds18x_get_global_locked();
  if (!ds18x) return true;
  ds18x->setWaitForConversion(false);
  mgos_ds18x_put_global_locked();

  int ms = mgos_sys_config_get_ds18x_ha_period() * 1000;
  if (ms < 5000) ms = 5000;
  if (mgos_set_timer(ms, MGOS_TIMER_REPEAT | MGOS_TIMER_RUN_NOW,
                     dsh_timer_period,
                     mgos_homeassistant_get_global()) == MGOS_INVALID_TIMER_ID)
    FNERR_RET(false, "mgos_set_timer(%u): %d", ms, MGOS_INVALID_TIMER_ID);
  return true;
}

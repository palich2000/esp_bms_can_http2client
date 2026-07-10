#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

#include <getopt.h>
#include <mosquitto.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "dfork.h"
#include "dlog.h"
#include "dmem.h"
#include "dpid.h"
#include "dsignal.h"

#include "json-c/json.h"
#include <curl/curl.h>

//------------------------------------------------------------------------------------------------------------
//
// Global handle Define
//
//------------------------------------------------------------------------------------------------------------
#define HOSTNAME_SIZE 256
#define CDIR "./"
char *progname = NULL;
char *hostname = NULL;
char *pathname = NULL;
const char *const application = "esp32-home-client";
static int do_exit = 0;
static int use_ibms = 0;
static const char *ibms_url = "http://192.168.0.126/api";

void publish_sensors(void);

void mqtt_publish_lwt(bool online);

void wd_sleep(int secs) {
  extern int do_exit;
  int s = secs;
  while (s > 0 && !do_exit) {
    sleep(1);
    s--;
  }
}

enum event_t {
  EVENT_UNKNOWN,
  EVENT_STATE,
  EVENT_LOG,
};

enum event_t get_event_type(const char *buf) {
  if (strstr(buf, "event: state")) {
    return EVENT_STATE;
  } else if (strstr(buf, "event: log")) {
    return EVENT_LOG;
  }
  return EVENT_UNKNOWN;
}

//{"power":0,"temp_tube": 24, "temp":24, "voltage": 55.5}

typedef struct {
  const char *id;
  const char *value_name;
  json_type tvalue;
  union {
    double dvalue;
    char *svalue;
  };
  int precision; // decimals for JSON output (double sensors)
  bool is_changed;
} sensor_t;
static sensor_t sensors[] = {
    //        {.id="sensor-jk-bms-interface_discharging_power",
    //        .value_name="power", .value=NAN},
    //        {.id="sensor-jk-bms-interface_charging_power",
    //        .value_name="power", .value=NAN},
    {.id = "sensor-jk-bms-interface_power_tube_temperature",
     .value_name = "temp_tube",
     .dvalue = NAN,
     .precision = 1,
     .tvalue = json_type_double},
    {.id = "sensor-jk-bms-interface_temperature_sensor_1",
     .value_name = "temp1",
     .dvalue = NAN,
     .precision = 1,
     .tvalue = json_type_double},
    {.id = "sensor-jk-bms-interface_temperature_sensor_2",
     .value_name = "temp2",
     .dvalue = NAN,
     .precision = 1,
     .tvalue = json_type_double},
    {.id = "sensor-jk-bms-interface_total_voltage",
     .value_name = "voltage",
     .dvalue = NAN,
     .precision = 2,
     .tvalue = json_type_double},
    {.id = "sensor-jk-bms-interface_capacity_remaining",
     .value_name = "soc",
     .dvalue = NAN,
     .precision = 0,
     .tvalue = json_type_double},
    {.id = "sensor-jk-bms-interface_current",
     .value_name = "current",
     .dvalue = NAN,
     .precision = 2,
     .tvalue = json_type_double},
    {.id = "sensor-jk-bms-interface_capacity_remaining_derived",
     .value_name = "capacity",
     .dvalue = NAN,
     .precision = 1,
     .tvalue = json_type_double},
    {.id = "text_sensor-jk-bms-interface_charging_status",
     .value_name = "charging_status",
     .svalue = NULL,
     .tvalue = json_type_string},
    {.id = "sensor-jk-bms-charged_energy",
     .value_name = "charged_energy",
     .dvalue = NAN,
     .precision = 2,
     .tvalue = json_type_double},
    {.id = "sensor-jk-bms-discharged_energy",
     .value_name = "discharged_energy",
     .dvalue = NAN,
     .precision = 2,
     .tvalue = json_type_double},
    {.id = "sensor-jk-bms-delta_cell_voltage",
     .value_name = "delta_cell_voltage",
     .dvalue = NAN,
     .precision = 3,
     .tvalue = json_type_double},
};

sensor_t *is_sensor(json_object *id) {
  if (!id) {
    return NULL;
  }
  for (size_t i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
    if (strcmp(sensors[i].id, json_object_get_string(id)) == 0) {
      return &sensors[i];
    }
  }
  return NULL;
}

size_t write_callback(const void *data, size_t size, size_t nmemb,
                      void *userp) {

  if (do_exit) {
    return 0;
  }

  (void)userp;

  const char *EOL = "\r\n";
  const char *p = data;
  size_t len = size * nmemb;
  // daemon_log(LOG_INFO, "Event");
  enum event_t event = EVENT_UNKNOWN;

  while (p < (char *)(data + len)) {
    char *p1 = strstr(p, EOL);
    if (p1 == NULL || p1 >= (char *)(data + len)) {
      break;
    }
    char *buf = calloc(p1 - p + 1, 1);
    if (buf) {
      memcpy(buf, p, p1 - p);
      enum event_t event_tmp = get_event_type(buf);
      if (event_tmp != EVENT_UNKNOWN) {
        event = event_tmp;
      }
      if (event == EVENT_LOG) {
        char *data = strstr(buf, "data:");
        if (data) {
          data += strlen("data:");
          daemon_log(LOG_INFO, "%s", data);
        }
      }
      if (event == EVENT_STATE) {
        char *data = strstr(buf, "data:");
        if (data) {
          data += strlen("data:");
          struct json_object *parsed_json;
          parsed_json = json_tokener_parse(data);
          if (parsed_json) {
            struct json_object *id;
            json_object_object_get_ex(parsed_json, "id", &id);
            sensor_t *sensor = is_sensor(id);
            if (sensor) {
              struct json_object *value;
              json_object_object_get_ex(parsed_json, "value", &value);
              json_type vtype = json_object_get_type(value);
              if (vtype == json_type_int) {
                // promote int to double
                vtype = json_type_double;
              }
              if (vtype == sensor->tvalue) {
                switch (vtype) {
                case json_type_double:
                  if (sensor->dvalue != json_object_get_double(value)) {
                    sensor->dvalue = json_object_get_double(value);
                    sensor->is_changed = true;
                    // daemon_log(LOG_INFO, "id: %s %.2f", sensor->value_name,
                    // sensor->value);
                    publish_sensors();
                  }
                  break;
                case json_type_string:
                  if (!sensor->svalue ||
                      strcmp(sensor->svalue, json_object_get_string(value)) !=
                          0) {
                    FREE(sensor->svalue);
                    sensor->svalue = strdup(json_object_get_string(value));
                    sensor->is_changed = true;
                    // daemon_log(LOG_INFO, "id: %s %s", sensor->value_name,
                    // sensor->svalue);
                    publish_sensors();
                  }
                  break;
                default:
                  break;
                }
              } else {
                daemon_log(LOG_ERR,
                           "Type mismatch for sensor %s expected %d but %d",
                           sensor->id, sensor->tvalue, vtype);
              }
            }
            json_object_put(parsed_json);
          } else {
            daemon_log(LOG_ERR, "JSON parse error: %s", data);
          }
        }
      }
      free(buf);
    }
    p = p1 + strlen(EOL);
  }
  return size * nmemb;
}

int debug_callback(CURL *handle, curl_infotype type, char *data, size_t size,
                   void *userptr) {
  (void)handle;
  (void)userptr;

  switch (type) {
  case CURLINFO_TEXT: {
    // daemon_log(LOG_INFO, "INFO: %.*s", (int) size - 1, data);
    char *str = alloca(size + 1);
    if (str) {
      memcpy(str, data, size);
      str[size] = 0;
      if (strstr(str, "Failed to connect")) {
        daemon_log(LOG_ERR, "Connection failed");
        mqtt_publish_lwt(false);
      }
    }
  } break;
  case CURLINFO_HEADER_IN: {
    daemon_log(LOG_INFO, "HEADER IN: %.*s", (int)size - 1, data);
    char *str = alloca(size + 1);
    if (str) {
      memcpy(str, data, size);
      str[size] = 0;
      if (strstr(str, "Connection: keep-alive")) {
        daemon_log(LOG_INFO, "Connected");
        mqtt_publish_lwt(true);
      }
    }
  } break;
  case CURLINFO_HEADER_OUT:
    daemon_log(LOG_INFO, "HEADER OUT: %.*s", (int)size - 1, data);
    break;
  case CURLINFO_DATA_IN:
    // printf("DATA IN: %zu bytes\n", size);
    break;
  case CURLINFO_DATA_OUT:
    // printf("DATA OUT: %zu bytes\n", size);
    break;
  case CURLINFO_SSL_DATA_IN:
  case CURLINFO_SSL_DATA_OUT:
    // printf("SSL DATA: %zu bytes\n", size);
    break;
  default:
    break;
  }
  return 0;
}

int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                      curl_off_t ultotal, curl_off_t ulnow) {
  (void)clientp;
  (void)ultotal;
  (void)ulnow;
  (void)dltotal;
  (void)dlnow;

  if (do_exit) {
    daemon_log(LOG_ERR, "Stopping curl_easy_perform()...");
    return 1;
  }
  return 0;
}

typedef struct {
  char *data;
  size_t size;
} ibms_buf_t;

static size_t write_callback_ibms(const void *ptr, size_t size, size_t nmemb,
                                  void *userp) {
  if (do_exit)
    return 0;
  ibms_buf_t *buf = (ibms_buf_t *)userp;
  size_t chunk = size * nmemb;
  char *tmp = realloc(buf->data, buf->size + chunk + 1);
  if (!tmp)
    return 0;
  memcpy(tmp + buf->size, ptr, chunk);
  buf->size += chunk;
  tmp[buf->size] = '\0';
  buf->data = tmp;
  return chunk;
}

typedef struct {
  const char *api_key;
  sensor_t *sensor;
} ibms_map_t;

static ibms_map_t ibms_map[] = {{"power_tube_temperature", &sensors[0]},
                                {"temperature_sensor_1", &sensors[1]},
                                {"temperature_sensor_2", &sensors[2]},
                                {"total_voltage", &sensors[3]},
                                {"capacity_remaining", &sensors[4]},
                                {"current", &sensors[5]},
                                {"capacity_remaining_derived", &sensors[6]},
                                {"charge_status", &sensors[7]},
                                {"charged_energy", &sensors[8]},
                                {"discharged_energy", &sensors[9]},
                                {"delta_cell_voltage", &sensors[10]}};

_Static_assert(sizeof(ibms_map) / sizeof(ibms_map[0]) == sizeof(sensors) /sizeof(sensors[0]),
               "ibms_map and sensors size mismatch");

static void ibms_parse_and_update(const char *json_str) {
  struct json_object *root = json_tokener_parse(json_str);
  if (!root) {
    daemon_log(LOG_ERR, "ibms: JSON parse error");
    return;
  }
  for (size_t i = 0; i < sizeof(ibms_map) / sizeof(ibms_map[0]); i++) {
    struct json_object *val;
    if (!json_object_object_get_ex(root, ibms_map[i].api_key, &val))
      continue;
    sensor_t *s = ibms_map[i].sensor;
    switch (s->tvalue) {
    case json_type_double: {
      double v = json_object_get_double(val);
      if (s->dvalue != v) {
        s->dvalue = v;
        s->is_changed = true;
      }
      break;
    }
    case json_type_string: {
      const char *sv = json_object_get_string(val);
      if (!s->svalue || strcmp(s->svalue, sv) != 0) {
        FREE(s->svalue);
        s->svalue = strdup(sv);
        s->is_changed = true;
      }
      break;
    }
    default:
      break;
    }
  }
  json_object_put(root);
  publish_sensors();
}

#define IBMS_POLL_INTERVAL 5

void *main_loop_ibms(void *UNUSED(param)) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    daemon_log(LOG_ERR, "ibms: curl_easy_init error");
    return NULL;
  }

  curl_easy_setopt(curl, CURLOPT_URL, ibms_url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback_ibms);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  // Prevent the poll thread from hanging forever on a stalled/half-open
  // connection (device reboot, NAT timeout, etc). Without these a single
  // curl_easy_perform() can block indefinitely and stop all polling.
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L); // max seconds to connect
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);       // max seconds per request
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);  // detect dead reused peer

  bool connected = false;

  while (!do_exit) {
    ibms_buf_t buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
      if (!connected) {
        connected = true;
        mqtt_publish_lwt(true);
      }
      if (buf.data) {
        ibms_parse_and_update(buf.data);
      }
    } else {
      daemon_log(LOG_ERR, "ibms: %s", curl_easy_strerror(res));
      if (connected) {
        connected = false;
        mqtt_publish_lwt(false);
      }
      if (!do_exit)
        wd_sleep(10);
    }

    free(buf.data);

    if (!do_exit)
      wd_sleep(IBMS_POLL_INTERVAL);
  }

  curl_easy_cleanup(curl);
  return NULL;
}

void *main_loop(void *UNUSED(param)) {
  CURLcode res = CURLE_OK;

  const char *url = "http://192.168.0.126/events";
  const char *username = "bms";
  const char *password = "can";

  while (!do_exit) {
    CURL *curl = curl_easy_init();
    if (!curl) {
      daemon_log(LOG_ERR, "curl_easy_init error");
      mqtt_publish_lwt(false);
      return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);

    char auth_header[64] = {0};
    snprintf(auth_header, sizeof(auth_header) - 1, "%s:%s", username, password);
    curl_easy_setopt(curl, CURLOPT_USERPWD, auth_header);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);

    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debug_callback);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);

    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      daemon_log(LOG_ERR, "Query error: %s", curl_easy_strerror(res));
      mqtt_publish_lwt(false);
      if (!do_exit) {
        wd_sleep(10);
      }
    }

    curl_easy_cleanup(curl);
  }

  return NULL;
}

#define ONLINE "Online"
#define OFFLINE "Offline"
#define STATE_PUBLISH_INTERVAL 60000  // 60 sec
#define SENSORS_PUBLISH_INTERVAL 5000 // 60 sec

#define MQTT_LWT_TOPIC "tele/%s/LWT"
#define MQTT_SENSOR_TOPIC "tele/%s/SENSOR"
#define MQTT_STATE_TOPIC "tele/%s/STATE"
#define FD_SYSTEM_TEMP_TMPL "/sys/class/thermal/thermal_zone%d/temp"
static int thermal_zone = 0;

typedef struct _client_info_t {
  struct mosquitto *m;
} t_client_info;

const char *mqtt_host = "192.168.0.106";
const char *mqtt_username = "owntracks";
const char *mqtt_password = "zhopa";
int mqtt_port = 8883;
int mqtt_keepalive = 60;
static t_client_info client_info;

static struct mosquitto *mosq = NULL;
static pthread_t mosq_th = 0;

uint64_t timeMillis(void) {
  struct timeval time;
  gettimeofday(&time, NULL);
  return time.tv_sec * 1000UL + time.tv_usec / 1000UL;
}

const char *create_topic(const char *template) {
  static __thread char buf[255] = {0};
  snprintf(buf, sizeof(buf) - 1, template, hostname);
  return buf;
}

void mqtt_publish_lwt(bool online) {
  const char *msg = online ? ONLINE : OFFLINE;
  int res;
  const char *topic = create_topic(MQTT_LWT_TOPIC);
  daemon_log(LOG_INFO, "publish %s: %s", topic, msg);
  if ((res = mosquitto_publish(mosq, NULL, topic, (int)strlen(msg), msg, 0,
                               true)) != 0) {
    DLOG_ERR("Can't publish to Mosquitto server %s", mosquitto_strerror(res));
  }
}

static void publish_state(void) {

  static uint64_t timer_publish_state = 0;

  if (timer_publish_state > timeMillis())
    return;
  else {
    timer_publish_state = timeMillis() + STATE_PUBLISH_INTERVAL;
  }

  time_t timer;
  char tm_buffer[26] = {};
  char buf[255] = {};
  struct tm *tm_info;
  struct sysinfo info;
  int res;

  time(&timer);
  tm_info = localtime(&timer);
  strftime(tm_buffer, 26, "%Y-%m-%dT%H:%M:%S", tm_info);

  if (!sysinfo(&info)) {
    int fd;
    char tmp_buf[20];
    memset(tmp_buf, ' ', sizeof(tmp_buf));
    char *f_name = NULL;
    asprintf(&f_name, FD_SYSTEM_TEMP_TMPL, thermal_zone);
    if ((fd = open(f_name, O_RDONLY)) < 0) {
      daemon_log(LOG_ERR, "%s : file open error!", __func__);
    } else {
      read(fd, buf, sizeof(tmp_buf));
      close(fd);
    }
    FREE(f_name);
    int temp_C = atoi(buf) / 1000;
    const char *topic = create_topic(MQTT_STATE_TOPIC);

    snprintf(buf, sizeof(buf) - 1,
             "{\"Time\":\"%s\", \"Uptime\": %ld, \"LoadAverage\":%.2f, "
             "\"CPUTemp\":%d}",
             tm_buffer, info.uptime / 3600, info.loads[0] / 65536.0, temp_C);

    daemon_log(LOG_INFO, "%s %s", topic, buf);
    if ((res = mosquitto_publish(mosq, NULL, topic, (int)strlen(buf), buf, 0,
                                 false)) != 0) {
      daemon_log(LOG_ERR, "Can't publish to Mosquitto server %s",
                 mosquitto_strerror(res));
    }
  }
}

void publish_sensors(void) {
  static uint64_t timer_publish_state = 0;
  uint64_t now = timeMillis();
  if (timer_publish_state > now) {
    static uint64_t last = 0;
    if (last < now) {
      last = now;
      return;
    }
    daemon_log(LOG_ERR, "Time loop detected");
    last = now;
    timer_publish_state = now + SENSORS_PUBLISH_INTERVAL;
  } else {
    timer_publish_state = now + SENSORS_PUBLISH_INTERVAL;
  }

  const char *topic = create_topic(MQTT_SENSOR_TOPIC);

  time_t timer;
  char tm_buffer[26] = {0};
  struct tm *tm_info;

  int res;

  time(&timer);
  tm_info = localtime(&timer);
  strftime(tm_buffer, 26, "%Y-%m-%dT%H:%M:%S", tm_info);
  json_object *j_root = json_object_new_object();
  json_object_object_add(j_root, "Time", json_object_new_string(tm_buffer));

  for (size_t i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
    sensors[i].is_changed = false;
    switch (sensors[i].tvalue) {
    case json_type_string: {
      if (sensors[i].svalue) {
        json_object_object_add(j_root, sensors[i].value_name,
                               json_object_new_string(sensors[i].svalue));
      }
    }
      continue;
    case json_type_double: {
      if (!isnan(sensors[i].dvalue)) {
        char buf[20] = {0};
        snprintf(buf, sizeof(buf) - 1, "%.*f", sensors[i].precision,
                 sensors[i].dvalue);
        json_object_object_add(
            j_root, sensors[i].value_name,
            json_object_new_double_s(sensors[i].dvalue, buf));
      }
    }
      continue;
    default:
      continue;
    }
  }

  const char *str = json_object_to_json_string_ext(
      j_root, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOZERO);
  daemon_log(LOG_INFO, "%s %s", topic, str);
  if ((res = mosquitto_publish(mosq, NULL, topic, (int)strlen(str), str, 0,
                               false)) != 0) {
    daemon_log(LOG_ERR, "Can't publish to Mosquitto server %s",
               mosquitto_strerror(res));
  }
  json_object_put(j_root);
}

static void *mosq_thread_loop(void *p) {
  t_client_info *info = (t_client_info *)p;
  daemon_log(LOG_INFO, "%s", __FUNCTION__);
  while (!do_exit) {
    int res = mosquitto_loop(info->m, 1000, 1);
    switch (res) {
    case MOSQ_ERR_SUCCESS:
      break;
    case MOSQ_ERR_NO_CONN: {
      int res = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
      if (res) {
        daemon_log(LOG_ERR, "Can't connect to Mosquitto server %s",
                   mosquitto_strerror(res));
        sleep(30);
      }
      break;
    }
    case MOSQ_ERR_INVAL:
    case MOSQ_ERR_NOMEM:
    case MOSQ_ERR_CONN_LOST:
    case MOSQ_ERR_PROTOCOL:
    case MOSQ_ERR_ERRNO:
      daemon_log(LOG_ERR, "%s %s %s", __FUNCTION__, strerror(errno),
                 mosquitto_strerror(res));
      mosquitto_disconnect(mosq);
      daemon_log(LOG_ERR, "%s disconnected", __FUNCTION__);
      sleep(10);
      daemon_log(LOG_ERR, "%s Try to reconnect", __FUNCTION__);
      int res = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
      if (res) {
        daemon_log(LOG_ERR, "%s Can't connect to Mosquitto server %s",
                   __FUNCTION__, mosquitto_strerror(res));
      } else {
        daemon_log(LOG_ERR, "%s Connected", __FUNCTION__);
      }

      break;
    default:
      daemon_log(LOG_ERR, "%s unkown error (%d) from mosquitto_loop",
                 __FUNCTION__, res);
      break;
    }
  }
  daemon_log(LOG_INFO, "%s finished", __FUNCTION__);
  pthread_exit(NULL);
}

static void on_publish(struct mosquitto *UNUSED(m), void *UNUSED(udata),
                       int UNUSED(m_id)) {
  // daemon_log(LOG_ERR, "-- published successfully");
}

static void on_subscribe(struct mosquitto *UNUSED(m), void *UNUSED(udata),
                         int UNUSED(mid), int UNUSED(qos_count),
                         const int *UNUSED(granted_qos)) {
  daemon_log(LOG_INFO, "-- subscribed successfully");
}

void on_log(struct mosquitto *UNUSED(mosq), void *UNUSED(userdata), int level,
            const char *str) {
  switch (level) {
    //    case MOSQ_LOG_DEBUG:
    //    case MOSQ_LOG_INFO:
    //    case MOSQ_LOG_NOTICE:
  case MOSQ_LOG_WARNING:
  case MOSQ_LOG_ERR: {
    daemon_log(LOG_ERR, "%i:%s", level, str);
  }
  }
}

static void on_connect(struct mosquitto *m, void *UNUSED(udata), int res) {
  daemon_log(LOG_INFO, "%s", __FUNCTION__);
  switch (res) {
  case 0:
    mosquitto_subscribe(m, NULL, "stat/+/POWER", 0);
    mqtt_publish_lwt(true);
    publish_state();
    break;
  case 1:
    DLOG_ERR("Connection refused (unacceptable protocol version).");
    break;
  case 2:
    DLOG_ERR("Connection refused (identifier rejected).");
    break;
  case 3:
    DLOG_ERR("Connection refused (broker unavailable).");
    break;
  default:
    DLOG_ERR("Unknown connection error. (%d)", res);
    break;
  }
  if (res != 0) {
    wd_sleep(10);
  }
}

static void on_message(struct mosquitto *UNUSED(m), void *UNUSED(udata),
                       const struct mosquitto_message *msg) {
  if (msg == NULL) {
    return;
  }

  daemon_log(LOG_INFO, "-- got message @ %s: (%d, QoS %d, %s) '%s'", msg->topic,
             msg->payloadlen, msg->qos, msg->retain ? "R" : "!r",
             (char *)msg->payload);
}

static void mosq_init() {

  bool clean_session = true;

  mosquitto_lib_init();
  char *tmp = alloca(strlen(progname) + strlen(hostname) + 2);
  strcpy(tmp, progname);
  strcat(tmp, "@");
  strcat(tmp, hostname);
  mosq = mosquitto_new(tmp, clean_session, &client_info);
  if (!mosq) {
    daemon_log(LOG_ERR, "mosq Error: Out of memory.");
  } else {
    client_info.m = mosq;
    mosquitto_log_callback_set(mosq, on_log);

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_publish_callback_set(mosq, on_publish);
    mosquitto_subscribe_callback_set(mosq, on_subscribe);
    mosquitto_message_callback_set(mosq, on_message);

    mosquitto_username_pw_set(mosq, mqtt_username, mqtt_password);
    mosquitto_will_set(mosq, create_topic(MQTT_LWT_TOPIC), strlen(OFFLINE),
                       OFFLINE, 0, true);
    daemon_log(LOG_INFO, "Try connect to Mosquitto server as %s", tmp);
    int res = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
    if (res) {
      daemon_log(LOG_ERR, "Can't connect to Mosquitto server %s",
                 mosquitto_strerror(res));
    }
    pthread_create(&mosq_th, NULL, mosq_thread_loop, &client_info);
  }
}

static void mosq_destroy() {
  pthread_join(mosq_th, NULL);
  if (mosq) {
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
  }
  mosquitto_lib_cleanup();
}

void usage(void) {
  fprintf(stderr,
          "Usage: %s [OPTIONS]\n"
          "\n"
          "Options:\n"
          "  -k, --command CMD       Send command to running daemon:\n"
          "                            reconfigure, shutdown, restart, check\n"
          "  -i, --ident NAME        Daemon ident / PID file name (default: "
          "esp32-home-client)\n"
          "  -f, --foreground        Run in foreground (don't daemonize)\n"
          "  -d, --debug             Enable debug logging (toggle with "
          "SIGUSR1/SIGUSR2)\n"
          "\n"
          "  -h, --mqtt-host HOST    MQTT broker hostname (default: "
          "192.168.0.106)\n"
          "  -p, --mqtt-port PORT    MQTT broker port (default: 8883)\n"
          "  -u, --mqtt-user USER    MQTT username (default: owntracks)\n"
          "  -P, --mqtt-password PWD MQTT password\n"
          "\n"
          "  -T, --thermal-zone N    CPU thermal zone index for STATE topic "
          "(default: 0)\n"
          "\n"
          "  -I, --ibms              Use iBMS polling mode (GET /api) instead "
          "of SSE /events\n"
          "  -U, --ibms-url URL      iBMS API URL (default: "
          "http://192.168.0.126/api)\n"
          "\n"
          "MQTT topics published: tele/{hostname}/LWT, tele/{hostname}/SENSOR, "
          "tele/{hostname}/STATE\n",
          progname);
  exit(1);
}

enum command_int_t {
  CMD_NONE = 0,
  CMD_RECONFIGURE,
  CMD_SHUTDOWN,
  CMD_RESTART,
  CMD_CHECK,
  CMD_NOT_FOUND = -1,
};

typedef int (*daemon_command_callback_t)(void *);

typedef struct daemon_command_t {
  const char *command_name;
  daemon_command_callback_t command_callback;
  int command_int;
} DAEMON_COMMAND_T;

int check_callback(void *UNUSED(param)) { return (10); }

int reconfigure_callback(void *UNUSED(param)) {

  if (daemon_pid_file_kill(SIGUSR1) < 0) {
    daemon_log(LOG_WARNING, "Failed to reconfiguring");
  } else {
    daemon_log(LOG_INFO, "OK");
  }
  return (10);
}

int shutdown_callback(void *UNUSED(param)) {
  int ret;
  daemon_log(LOG_INFO, "Try to shutdown self....");
  if ((ret = daemon_pid_file_kill_wait(SIGINT, 10)) < 0) {
    daemon_log(LOG_WARNING, "Failed to shutdown daemon %d %s", errno,
               strerror(errno));
    daemon_log(LOG_WARNING, "Try to terminating self....");
    if (daemon_pid_file_kill_wait(SIGKILL, 0) < 0) {
      daemon_log(LOG_WARNING, "Failed to killing daemon %d %s", errno,
                 strerror(errno));
    } else {
      daemon_log(LOG_WARNING, "Daemon terminated");
    }
  } else
    daemon_log(LOG_INFO, "OK");
  return (10);
}

int restart_callback(void *UNUSED(param)) {
  shutdown_callback(NULL);
  return (0);
}

const DAEMON_COMMAND_T daemon_commands[] = {
    {.command_name = "reconfigure",
     .command_callback = reconfigure_callback,
     .command_int = CMD_RECONFIGURE},
    {.command_name = "shutdown",
     .command_callback = shutdown_callback,
     .command_int = CMD_SHUTDOWN},
    {.command_name = "restart",
     .command_callback = restart_callback,
     .command_int = CMD_RESTART},
    {.command_name = "check",
     .command_callback = check_callback,
     .command_int = CMD_CHECK},
};

int main(int argc, char *const *argv) {
  int flags;
  int daemonize = true;
  int debug = 0;
  char *command = NULL;
  pid_t pid;
  pthread_t main_th = 0;

  int fd, sel_res;

  daemon_pid_file_ident = daemon_log_ident = application;

  tzset();

  if ((progname = strrchr(argv[0], '/')) == NULL)
    progname = argv[0];
  else
    ++progname;

  if (strrchr(argv[0], '/') == NULL)
    pathname = xstrdup(CDIR);
  else {
    pathname = xmalloc(strlen(argv[0]) + 1);
    strncpy(pathname, argv[0], (size_t)(strrchr(argv[0], '/') - argv[0]) + 1);
  }

  if (chdir(pathname) < 0) {
    daemon_log(LOG_ERR, "chdir error: %s", strerror(errno));
  }

  FREE(pathname);

  pathname = get_current_dir_name();

  hostname = strdup("main_battery");

  daemon_log_upto(LOG_INFO);
  daemon_log(LOG_INFO, "%s %s", pathname, progname);

  static struct option long_options[] = {
      {"command", required_argument, 0, 'k'},
      {"ident", required_argument, 0, 'i'},
      {"foreground", no_argument, 0, 'f'},
      {"debug", no_argument, 0, 'd'},
      {"mqtt-host", required_argument, 0, 'h'},
      {"mqtt-port", required_argument, 0, 'p'},
      {"mqtt-user", required_argument, 0, 'u'},
      {"mqtt-password", required_argument, 0, 'P'},
      {"thermal-zone", required_argument, 0, 'T'},
      {"ibms", no_argument, 0, 'I'},
      {"ibms-url", required_argument, 0, 'U'},
      {0, 0, 0, 0}};

  while ((flags = getopt_long(argc, argv, "k:i:fdh:p:u:P:T:IU:", long_options,
                              NULL)) != -1) {

    switch (flags) {
    case 'i': {
      daemon_pid_file_ident = daemon_log_ident = xstrdup(optarg);
      break;
    }
    case 'f': {
      daemonize = false;
      break;
    }
    case 'd': {
      debug++;
      daemon_log_upto(LOG_DEBUG);
      break;
    }
    case 'k': {
      command = xstrdup(optarg);
      break;
    }
    case 'p': {
      mqtt_port = atoi(optarg);
      break;
    }
    case 'u': {
      mqtt_username = xstrdup(optarg);
      break;
    }
    case 'P': {
      mqtt_password = xstrdup(optarg);
      break;
    }
    case 'h': {
      mqtt_host = xstrdup(optarg);
      break;
    }
    case 'T': {
      thermal_zone = atoi(optarg);
      break;
    }
    case 'I': {
      use_ibms = 1;
      break;
    }
    case 'U': {
      ibms_url = xstrdup(optarg);
      break;
    }
    default: {
      usage();
      break;
    }
    }
  }

  if (debug) {
    daemon_log(LOG_DEBUG, "**************************");
    daemon_log(LOG_DEBUG, "* WARNING !!! Debug mode *");
    daemon_log(LOG_DEBUG, "**************************");
  }
  daemon_log(LOG_INFO, "%s compiled at [%s %s] started", application, __DATE__,
             __TIME__);
  daemon_log(LOG_INFO, "*******************************************************"
                       "********************");
  daemon_log(LOG_INFO, "pid file: %s", daemon_pid_file_proc());

  if (command) {
    int r = CMD_NOT_FOUND;
    for (unsigned int i = 0;
         i < (sizeof(daemon_commands) / sizeof(daemon_commands[0])); i++) {
      if ((strcasecmp(command, daemon_commands[i].command_name) == 0) &&
          (daemon_commands[i].command_callback)) {
        if ((r = daemon_commands[i].command_callback(pathname)) != 0)
          exit(abs(r - 10));
      }
    }
    if (r == CMD_NOT_FOUND) {
      daemon_log(LOG_ERR, "command \"%s\" not found.", command);
      usage();
    }
  }

  FREE(command);

  /* initialize PRNG */
  srand((unsigned int)time(NULL));

  if ((pid = daemon_pid_file_is_running()) >= 0) {
    daemon_log(LOG_ERR, "Daemon already running on PID file %u", pid);
    return 1;
  }

  daemon_log(LOG_INFO, "Make a daemon");

  daemon_retval_init();
  if ((daemonize) && ((pid = daemon_fork()) < 0)) {
    return 1;
  } else if ((pid) && (daemonize)) {
    int ret;
    if ((ret = daemon_retval_wait(20)) < 0) {
      daemon_log(LOG_ERR,
                 "Could not recieve return value from daemon process.");
      return 255;
    }
    if (ret == 0) {
      daemon_log(LOG_INFO, "Daemon started.");
    } else {
      daemon_log(LOG_ERR, "Daemon dont started, returned %i as return value.",
                 ret);
    }
    return ret;
  } else {

    if (daemon_pid_file_create() < 0) {
      daemon_log(LOG_ERR, "Could not create PID file (%s).", strerror(errno));
      daemon_retval_send(1);
      goto finish;
    }

    if (daemon_signal_init(/*SIGCHLD,*/ SIGINT, SIGTERM, SIGQUIT, SIGHUP,
                           SIGUSR1, SIGUSR2, SIGHUP, /*SIGSEGV,*/ 0) < 0) {
      daemon_log(LOG_ERR, "Could not register signal handlers (%s).",
                 strerror(errno));
      daemon_retval_send(1);
      goto finish;
    }

    daemon_retval_send(0);
    daemon_log(LOG_INFO, "%s compiled at [%s %s] started", application,
               __DATE__, __TIME__);

    struct rlimit core_lim;

    if (getrlimit(RLIMIT_CORE, &core_lim) < 0) {
      daemon_log(LOG_ERR, "getrlimit RLIMIT_CORE error:%s", strerror(errno));
    } else {
      daemon_log(LOG_INFO, "core limit is cur:%2ld max:%2ld", core_lim.rlim_cur,
                 core_lim.rlim_max);
      core_lim.rlim_cur = ULONG_MAX;
      core_lim.rlim_max = ULONG_MAX;
      if (setrlimit(RLIMIT_CORE, &core_lim) < 0) {
        daemon_log(LOG_ERR, "setrlimit RLIMIT_CORE error:%s", strerror(errno));
      } else {
        daemon_log(LOG_INFO, "core limit set cur:%2ld max:%2ld",
                   core_lim.rlim_cur, core_lim.rlim_max);
      }
    }

    mosq_init();

    pthread_create(&main_th, NULL, use_ibms ? main_loop_ibms : main_loop, NULL);

    fd_set fds;
    FD_ZERO(&fds);
    fd = daemon_signal_fd();
    FD_SET(fd, &fds);

    while (!do_exit) {
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 100000;
      fd_set fds2 = fds;
      if ((sel_res = select(FD_SETSIZE, &fds2, 0, 0, &tv)) < 0) {

        if (errno == EINTR)
          continue;

        daemon_log(LOG_ERR, "select() error:%d %s", errno, strerror(errno));
        break;
      }
      if (FD_ISSET(fd, &fds2)) {
        int sig;

        if ((sig = daemon_signal_next()) <= 0) {
          daemon_log(LOG_ERR, "daemon_signal_next() failed.");
          break;
        }

        switch (sig) {
        case SIGCHLD: {
          int ret = 0;
          daemon_log(LOG_INFO, "SIG_CHLD");
          wait(&ret);
          daemon_log(LOG_INFO, "RET=%d", ret);
        } break;

        case SIGINT:
        case SIGQUIT:
        case SIGTERM:
          daemon_log(LOG_WARNING, "Got SIGINT, SIGQUIT or SIGTERM");
          do_exit = true;
          break;

        case SIGUSR1: {
          daemon_log(LOG_WARNING, "Got SIGUSR1");
          daemon_log(LOG_WARNING,
                     "Enter in debug mode, to stop send me USR2 signal");
          daemon_log_upto(LOG_DEBUG);
          break;
        }
        case SIGUSR2: {
          daemon_log(LOG_WARNING, "Got SIGUSR2");
          daemon_log(LOG_WARNING, "Leave debug mode");
          daemon_log_upto(LOG_INFO);
          break;
        }
        case SIGHUP:
          daemon_log(LOG_WARNING, "Got SIGHUP");
          break;

        case SIGSEGV:
          daemon_log(LOG_ERR, "Seg fault. Core dumped to /tmp/core.");
          if (chdir("/tmp") < 0) {
            daemon_log(LOG_ERR, "Chdir to /tmp error: %s", strerror(errno));
          }
          signal(sig, SIG_DFL);
          kill(getpid(), sig);
          break;

        default:
          daemon_log(LOG_ERR, "UNKNOWN SIGNAL:%s", strsignal(sig));
          break;
        }
      }
    }
  }

finish:
  mqtt_publish_lwt(false);
  daemon_log(LOG_INFO, "Exiting...");
  mosq_destroy();
  pthread_join(main_th, NULL);
  FREE(hostname);
  FREE(pathname);
  daemon_retval_send(-1);
  daemon_signal_done();
  daemon_pid_file_remove();
  daemon_log(LOG_INFO, "Exit");
  exit(0);
}
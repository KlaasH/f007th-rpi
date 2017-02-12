/*
 * F007TH_push_to_rest.cpp
 *
 * Push sensors readings to remote server via REST API
 *
 * Created on: February 4, 2017
 * @author Alex Konshin <akonshin@gmail.com>
 */

#include "RFReceiver.hpp"
#include <curl/curl.h>

#include "SensorsData.hpp"

#define SERVER_TYPE_REST 0

enum ServerType {REST, InfluxDB};

static bool send(ReceivedMessage& message, const char* url, ServerType server_type, int changed, char* data_buffer, char* response_buffer, int verbosity, FILE* log);

static void help() {
  fputs(
    "(c) 2017 Alex Konshin\n"\
    "Receive data from sensors Ambient Weather F007TH then send it to remote server via REST API.\n\n"\
    "--gpio, -g\n"\
    "    Value is GPIO pin number (default is 27) as defined on page http://abyz.co.uk/rpi/pigpio/index.html\n"\
    "--send-to, -s\n"\
    "    Parameter value is server URL.\n"\
    "--server-type, -t\n"\
    "    Parameter value is server type. Possible values are REST (default) or InfluxDB.\n"\
    "--all, -A\n"\
    "    Send all data. Only changed and valid data is sent by default.\n"\
    "--log-file, -l\n"\
    "    Parameter is a path to log file.\n"\
    "--verbose, -v\n"\
    "    Verbose output.\n"\
    "--more_verbose, -V\n"\
    "    More verbose output.\n",
    stderr
  );
  fflush(stderr);
  fclose(stderr);
  exit(1);
}

int main(int argc, char *argv[]) {

  if ( argc==1 ) help();

  const char* log_file_path = NULL; // TODO real logging is not implemented yet
  const char* server_url = NULL;
  int gpio = 27;
  ServerType server_type = REST;
  int verbosity = 0;

  bool changes_only = true;

  const char* short_options = "g:s:Al:vVt:T";
  const struct option long_options[] = {
      { "gpio", required_argument, NULL, 'g' },
      { "send-to", required_argument, NULL, 's' },
      { "server-type", required_argument, NULL, 't' },
      { "all", no_argument, NULL, 'A' },
      { "log-file", required_argument, NULL, 'l' },
      { "verbose", no_argument, NULL, 'v' },
      { "more_verbose", no_argument, NULL, 'V' },
      { "statistics", no_argument, NULL, 'T' },
      { NULL, 0, NULL, 0 }
  };

  long long_value;
  while (1) {
    int c = getopt_long(argc, argv, short_options, long_options, NULL);
    if (c == -1) break;

    switch (c) {

    case 'g':
      long_value = strtol(optarg, NULL, 10);
      if (long_value<=0 || long_value>53) {
        fprintf(stderr, "ERROR: Invalid GPIO pin number \"%s\".\n", optarg);
        help();
      }
      gpio = (int)long_value;
      break;

    case 'A':
      changes_only = false;
      break;

    case 'v':
      verbosity |= VERBOSITY_INFO;
      break;

    case 'l':
      log_file_path = optarg;
      break;

    case 's':
      server_url = optarg;
      break;

    case 't':
      if (strcasecmp(optarg, "REST") == 0) {
        server_type = REST;
      } else if (strcasecmp(optarg, "InfluxDB") == 0) {
        server_type = InfluxDB;
      } else {
        fprintf(stderr, "ERROR: Unknown server type \"%s\".\n", optarg);
        help();
      }
      break;

    case 'T':
      verbosity |= VERBOSITY_PRINT_STATISTICS;
      break;

    case 'V':
      verbosity |= VERBOSITY_INFO | VERBOSITY_PRINT_JSON | VERBOSITY_PRINT_CURL | VERBOSITY_PRINT_UNDECODED | VERBOSITY_PRINT_DETAILS;
      break;

    case '?':
      help();
      break;

    default:
      fprintf(stderr, "ERROR: Unknown option \"%s\".\n", argv[optind]);
      help();
    }
  }

  if (optind < argc) {
    if (optind != argc-1) help();
    server_url = argv[optind];
  }

  if (server_url == NULL || server_url[0]=='\0') {
    fputs("ERROR: Server URL must be specified (options --send-to or -s).\n", stderr);
    help();
  }
  //TODO support UNIX sockets for InfluxDB
  if (strncmp(server_url, "http://", 7) != 0 && strncmp(server_url, "https://", 8)) {
    fputs("ERROR: Server URL must be HTTP or HTTPS.\n", stderr);
    help();
  }

  if (log_file_path == NULL || log_file_path[0]=='\0') {
    log_file_path = "f007th-send.log";
  }

  FILE* log = fopen(log_file_path, "w+");

  curl_global_init(CURL_GLOBAL_ALL);

  char* data_buffer = (char*)malloc(SEND_DATA_BUFFER_SIZE*sizeof(char));
  char* response_buffer = (char*)malloc(SERVER_RESPONSE_BUFFER_SIZE*sizeof(char));

  SensorsData sensorsData;

  RFReceiver receiver(gpio);

  ReceivedMessage message;

  receiver.enableReceive();

  if ((verbosity&VERBOSITY_PRINT_STATISTICS) != 0)
    receiver.printStatisticsPeriodically(1000); // print statistics every second

  if ((verbosity&VERBOSITY_INFO) != 0) fputs("Receiving data...\n", stderr);
  while(!receiver.isStopped()) {

    if (receiver.waitForMessage(message)) {
      if (receiver.isStopped()) break;

      if ((verbosity&VERBOSITY_INFO) != 0) {
        message.print(stdout, verbosity);
        if (message.print(log, verbosity)) fflush(log);
      }

      if (message.isEmpty()) {
        fputs("ERROR: Missing data.\n", stderr);
      } else if (message.isUndecoded()) {
        if ((verbosity&VERBOSITY_INFO) != 0)
          fprintf(stderr, "Could not decode the received data (error %04x).\n", message.getDecodingStatus());
      } else {
        bool isValid = message.isValid();
        int changed = isValid ? sensorsData.update(message.getData()) : 0;
        if (changed == 0 && !changes_only && (isValid || server_type==REST))
          changed = TEMPERATURE_IS_CHANGED | HUMIDITY_IS_CHANGED | BATTERY_STATUS_IS_CHANGED;
        if (changed != 0) {
          if (!send(message, server_url, server_type, changed, data_buffer, response_buffer, verbosity, log) && (verbosity&VERBOSITY_INFO) != 0)
            fputs("No data was sent to server.\n", stderr);
        } else {
          if ((verbosity&VERBOSITY_INFO) != 0) {
            if (!isValid)
              fputs("Data is not valid and is not sent to server.\n", stderr);
            else
              fputs("Data is not changed and is not sent to server.\n", stderr);
          }
        }
      }

    }

    if (receiver.checkAndResetTimerEvent()) {
      receiver.printStatistics();
    }
  }
  if ((verbosity&VERBOSITY_INFO) != 0) fputs("\nExiting...\n", stderr);

  // finally
  free(data_buffer);
  free(response_buffer);
  fclose(log);
  curl_global_cleanup();

  exit(0);
}

typedef struct PutData {
  uint8_t* data;
  size_t len;
  uint8_t* response_data;
  size_t response_len;
  bool verbose;
} PutData;
/*
static size_t read_callback(void *ptr, size_t size, size_t nmemb, struct PutData* data) {
  size_t curl_size = nmemb * size;
  size_t to_copy = (data->len < curl_size) ? data->len : curl_size;
  memcpy(ptr, data->data, to_copy);
  if (data->verbose) fprintf(stderr, "sending %d bytes...\n", to_copy);
  data->len -= to_copy;
  data->data += to_copy;
  return to_copy;
}
*/
static size_t write_callback(void *ptr, size_t size, size_t nmemb, struct PutData* data) {
  size_t curl_size = nmemb * size;
  size_t to_copy = (data->response_len < curl_size) ? data->response_len : curl_size;
  if (data->verbose) fprintf(stderr, "receiving %d bytes...\n", to_copy);
  if (to_copy > 0) {
    memcpy(data->response_data, ptr, to_copy);
    data->response_len -= to_copy;
    data->response_data += to_copy;
    return to_copy;
  }
  return size;
}

bool send(ReceivedMessage& message, const char* url, ServerType server_type, int changed, char* data_buffer, char* response_buffer, int verbosity, FILE* log) {
  bool verbose = (verbosity&VERBOSITY_PRINT_DETAILS) != 0;
  if (verbose) fputs("===> called send()\n", stderr);

  data_buffer[0] = '\0';
  int data_size;
  if (server_type == InfluxDB) {
    data_size = message.influxDB(data_buffer, SEND_DATA_BUFFER_SIZE, changed, (verbosity&VERBOSITY_PRINT_JSON) != 0);
  } else {
    data_size = message.json(data_buffer, SEND_DATA_BUFFER_SIZE, true, (verbosity&VERBOSITY_PRINT_JSON) != 0);
  }
  if (data_size <= 0) {
    if (verbose) fputs("===> return from send() without sending because output data was not generated\n", stderr);
    return false;
  }

  struct PutData data;
  data.data = (uint8_t*)data_buffer;
  data.len = data_size;
  data.response_data = (uint8_t*)response_buffer;
  data.response_len = SERVER_RESPONSE_BUFFER_SIZE;
  data.verbose = verbose;
  response_buffer[0] = '\0';

  CURL* curl = curl_easy_init();  // get a curl handle
  if (!curl) {
    fputs("ERROR: Failed to get curl handle.\n", stderr);
    if (verbose) fputs("===> return from send() without sending because failed to initialize curl\n", stderr);
    return false;
  }
  if ((verbosity&VERBOSITY_PRINT_CURL) != 0) curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

  struct curl_slist *headers = NULL;
  if (server_type == REST) {
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "charsets: utf-8");
    headers = curl_slist_append(headers, "Connection: close");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  } if (server_type == InfluxDB) {
    headers = curl_slist_append(headers, "Content-Type:"); // do not set content type
    headers = curl_slist_append(headers, "Accept:");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
//  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  if (server_type == InfluxDB)
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
  else
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
//  curl_easy_setopt(curl, CURLOPT_READFUNCTION, &read_callback);
//  curl_easy_setopt(curl, CURLOPT_READDATA, &data);
//  curl_off_t size = json_size;
//  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, size);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data_buffer);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    fprintf(stderr, "ERROR: Sending data to %s failed: %s\n", url, curl_easy_strerror(rc));
    fprintf(log, "ERROR: Sending data to %s failed: %s\n", url, curl_easy_strerror(rc));
  }
  if (verbose && response_buffer[0] != '\0') {
    if (data.response_len <= 0) data.response_len = data.response_len-1;
    response_buffer[SERVER_RESPONSE_BUFFER_SIZE-data.response_len] = '\0';
    fputs(response_buffer, stderr);
    fputc('\n', stderr);
  }
  bool success = rc == CURLE_OK;
  long http_code = 0;
  curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code != (server_type == InfluxDB ? 204 : 200)) {
    success = false;
    if (http_code == 0)
      fprintf(stderr, "ERROR: Failed to connect to server %s\n", url);
    else
      fprintf(stderr, "ERROR: Got HTTP status code %ld.\n", http_code);
    if (verbose) {
      fputs(response_buffer, log);
      fputc('\n', log);
    }
    fprintf(log, "ERROR: Got HTTP status code %ld.\n", http_code);
    fputs(response_buffer, log);
    fputc('\n', log);
  } else if (rc == CURLE_ABORTED_BY_CALLBACK) {
    fputs("ERROR: HTTP request was aborted.\n", stderr);
    fputs("ERROR: HTTP request was aborted.\n", log);
  }
  curl_easy_cleanup(curl);
  if (headers != NULL) curl_slist_free_all(headers);
  if (verbose) fputs("===> return from send()\n", stderr);
  return success;
}



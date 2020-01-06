/* Example of using Web Thing Server

   Krzysztof Zurek, krzurek@gmail.com
   Jan 1 2020
   MIT licence
*/
#include <stdio.h>
#include <sys/param.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mdns.h"
//#include "esp_event_loop.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_attr.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/apps/sntp.h"

#include "simple_web_thing_server.h"
#include "web_thing_softap.h"
#include "web_thing_mdns.h"
#include "thing_button.h"
#include "thing_blinking_led.h"

//NVS wifi delete button
#define GPIO_DELETE_BUTTON		27
#define GPIO_DELETE_BUTTON_MASK	(1ULL << GPIO_DELETE_BUTTON)

#define ESP_INTR_FLAG_DEFAULT	0

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;

//wifi station configuration data
char mdns_hostname[65];

const int IP4_CONNECTED_BIT = BIT0;
const int IP6_CONNECTED_BIT = BIT1;

static int irq_counter = 0;

xSemaphoreHandle DRAM_ATTR delete_button_sem;
static bool DRAM_ATTR nvs_delete_button_ready = false;
static xTaskHandle ap_server_task_handle;

//wifi data
/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static const char *TAG_WIFI = "wifi station";
static int s_retry_num = 0;

static void chipInfo(void);
//network functions
static void event_handler(void* arg, esp_event_base_t event_base, 
                                int32_t event_id, void* event_data);
void wifi_init_sta(char *ssid, char *pass);
static void ioInit(void);
void init_delete_button(void);
void init_delete_button_io(void);
void delete_button_fun(void *pvParameter);
void init_nvs(void);
static void init_sntp(void);

//other tasks
bool thing_server_loaded = false;
bool node_restart = false;
static bool node_is_station = false;
void init_things(void);


/***************************************************************
 *
 * MAIN ENTRY POINT
 *
 * **************************************************************/
void app_main(){
	time_t now;
	struct tm timeinfo;
	bool time_zone_set = false;
	char time_buffer[100];

	// Initialize NVS
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		// NVS partition was truncated and needs to be erased
		// Retry nvs_flash_init
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK( err );

	//chip information
	chipInfo();
	ioInit();
	init_delete_button();
	
	//initialize things, properties etc.
	init_things();

	init_nvs();

	nvs_delete_button_ready = true;

	//start here additional non-network tasks
	vTaskDelay(100 / portTICK_PERIOD_MS);
	
	int32_t heap, prev_heap, i = 0;
	heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
	prev_heap = heap;
	printf("%i, free heap: %i, irq: %i\n", i, heap, irq_counter);

	while (node_restart == false) {
		i++;
		vTaskDelay(2000 / portTICK_PERIOD_MS);
		
		if (node_is_station == true){
			//read time
			if (time_zone_set == false){
				//setenv("TZ", "CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00", 1);
				//without daylight savings
				setenv("TZ", "CET", 1);
				tzset();
				time_zone_set = true;
			}
			
			heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
			if (heap != prev_heap){
				time(&now);
				localtime_r(&now, &timeinfo);
				strftime(time_buffer, sizeof(time_buffer), "%Y/%m/%d %H:%M:%S", &timeinfo);
				printf("%s, free heap: %i, irq: %i\n", time_buffer, heap, irq_counter);
				prev_heap = heap;
			}
		}
	}

	printf("Restarting now.\n");
	fflush(stdout);
	esp_wifi_stop();
	esp_restart();
}


/********************************************************
 *
 * initialization of all things
 *
 * *****************************************************/
void init_things(){
	//build up the thing
	puts("Web Thing Server is starting\n");

	root_node_init();

	//initialize iot button
	add_thing_to_server(init_button());
	//initialize iot blinking led
	add_thing_to_server(init_blinking_led());
}


/***************************************************************
 *
 *
 *
 * **************************************************************/
static void event_handler(void* arg, esp_event_base_t event_base, 
                                int32_t event_id, void* event_data){ 

	if (event_base == WIFI_EVENT){
		switch(event_id) {
	    	case WIFI_EVENT_STA_START:
	    		//ready to connect with AP
	    		esp_wifi_connect();
	    		break;

    		case WIFI_EVENT_STA_STOP:
	    		ESP_LOGI(TAG_WIFI, "wifi is stopped");
	    		s_retry_num = 0;
	    		break;

    		case WIFI_EVENT_STA_CONNECTED:
	    		/* enable ipv6 */
	    	    tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_STA);
	    	    break;

    		case WIFI_EVENT_STA_DISCONNECTED:
	    		//never give up!
    			vTaskDelay(1000 / portTICK_PERIOD_MS);
	    		esp_wifi_connect();
	    		xEventGroupClearBits(wifi_event_group, IP4_CONNECTED_BIT | IP6_CONNECTED_BIT);
	    		s_retry_num++;
	    		ESP_LOGI(TAG_WIFI,"retry to connect to the AP, %i", s_retry_num);
	    		break;
		    	
	    	default:
    			break;
    	}
    }
	else if (event_base == IP_EVENT){
	    if (event_id == IP_EVENT_STA_GOT_IP){
	    	ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
	    	ESP_LOGI(TAG_WIFI, "got ip:%s", ip4addr_ntoa(&event->ip_info.ip));
	    	s_retry_num = 0;
	    	xEventGroupSetBits(wifi_event_group, IP4_CONNECTED_BIT);
	    	if (thing_server_loaded == false){
	    		//initialize web thing server
	    		start_web_thing_server(8080, mdns_hostname, DOMAIN);
	    		thing_server_loaded = true;
	    		//initialize sntp client
	    		init_sntp();
	    		node_is_station = true;
	    	}
	    }
	    else if (event_id == IP_EVENT_GOT_IP6){
    		xEventGroupSetBits(wifi_event_group, IP6_CONNECTED_BIT);
    	}
    }
}


/**************************************************************
 *
 * wifi initialization
 *
 * ************************************************************/
void wifi_init_sta(char *ssid, char *pass){
	wifi_config_t wifi_config;

    wifi_event_group = xEventGroupCreate();

    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    
    strcpy((char *)wifi_config.sta.ssid, ssid);
    strcpy((char *)wifi_config.sta.password, pass);
    wifi_config.sta.bssid_set = false;
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.threshold.rssi = -100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );

    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK){
    	ESP_LOGI(TAG_WIFI, "wifi_init_sta finished.");
    	//turn off power savings
    	esp_wifi_set_ps(WIFI_PS_NONE);
    	//power savings not correctly works with IoT Mozilla Gateway
    	//thing sometimes loses connection with gateway
    	//esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
    else{
    	ESP_LOGI(TAG_WIFI, "connection to AP failed");
    	esp_wifi_deinit();
    }
}


/****************************************************************
 *
 * start NVS on default partition labeled "nvs" (24kB)
 *
 * **************************************************************/
void init_nvs(void){
	esp_err_t err;
	nvs_handle storage_handle = 0;
	
	char wifi_ssid[33];
	char wifi_pass[65];

	// Open
	printf("\n");
	printf("Opening Non-Volatile Storage (NVS) handle... ");

	err = nvs_open("storage", NVS_READWRITE, &storage_handle);
	if (err != ESP_OK) {
		printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
	}
	else {
		uint32_t ssid_len = 0, pass_len = 0, mdns_len = 0;

		printf("Done\n");

		// Read
		printf("Reading ssid and password from NVS ... ");
		esp_err_t err1 = nvs_get_str(storage_handle, "ssid", NULL, &ssid_len);
		esp_err_t err2 = nvs_get_str(storage_handle, "pass", NULL, &pass_len);
		esp_err_t err3 = nvs_get_str(storage_handle, "mdns_host", NULL, &mdns_len);
		printf("Done\n");
		printf("errors: %i, %i, %i, \n", err1, err2, err3);

		if ((err1 == ESP_OK) && (err2 == ESP_OK) && (ssid_len > 0) && (pass_len > 0)
				&& (err3 == ESP_OK) && (mdns_len > 0)){
			//password and ssid is defined, connect to network and start
			//web thing server
			if ((ssid_len < 33) && (pass_len < 65) && (mdns_len < 65)){
				nvs_get_str(storage_handle, "ssid", wifi_ssid, &ssid_len);
				nvs_get_str(storage_handle, "pass", wifi_pass, &pass_len);
				nvs_get_str(storage_handle, "mdns_host", mdns_hostname, &mdns_len);
				wifi_ssid[ssid_len - 1] = 0;
				wifi_pass[pass_len - 1] = 0;
				mdns_hostname[mdns_len - 1] = 0;
				//without printf below node failed constantly in connecting
				//to AP! - what is it?
				vTaskDelay(5 / portTICK_PERIOD_MS); //5ms wait solves problem temporaryly

				//initialize wifi
				wifi_init_sta(wifi_ssid, wifi_pass);
				initialise_mdns(mdns_hostname, false);
			}
			else{
				printf("ssid, password or hostname too long, %i, %i, %i\n",
						ssid_len, pass_len, mdns_len);
			}
		}
		else{
			//ssid, password or node name not defined
			//start AP and server with page for defining these parameters
			wifi_init_softap();
			//initialize mDNS service
			initialise_mdns(NULL, true);
			node_is_station = false;
			//start server
			xTaskCreate(ap_server_task, "ap_server_task", 1024*4, &node_restart, 1, &ap_server_task_handle);
		}
		err = nvs_commit(storage_handle);
		printf((err != ESP_OK) ? "Commit failed!\n" : "Commit done\n");

		// Close
		nvs_close(storage_handle);
	}
	printf("\n");
}


/*************************************************************
 *
 * Delete wifi SSID and PASS and restart node in AP mode
 * (used to connect to other wifi network)
 *
 * ************************************************************/
void delete_button_fun(void *pvParameter){
	esp_err_t err;
	nvs_handle storage_handle = 0;

	printf("Button task is ready\n");

	for(;;){
		//wait for button pressed
		xSemaphoreTake(delete_button_sem, portMAX_DELAY);
		printf("Delete wifi data in NVS\n");
		irq_counter++;
		//wait a bit to avoid button vibration
		vTaskDelay(200 / portTICK_PERIOD_MS);
		int lev = gpio_get_level(GPIO_DELETE_BUTTON);

		if (lev == 0){
			err = nvs_open("storage", NVS_READWRITE, &storage_handle);
			if (err == ESP_OK){

				nvs_erase_key(storage_handle, "ssid");
				nvs_erase_key(storage_handle, "pass");
				nvs_erase_key(storage_handle, "mdns_host");

				printf("Committing updates in NVS ... ");
				err = nvs_commit(storage_handle);
				printf((err != ESP_OK) ? "Failed!\n" : "Done\n");

				nvs_close(storage_handle);
				node_restart = true;
			}
			else{
				printf("NVS failed to open: %s\n", esp_err_to_name(err));
			}
		}

		//wait a bit to avoid button vibration
		vTaskDelay(1000 / portTICK_PERIOD_MS);

		nvs_delete_button_ready = true;
	}
}


/***************************************************
 *
 * init I/O
 *
 **************************************************/
static void ioInit(void){
	//gpio_config_t io_conf;

	//init READY LED
	//disable interrupt
	//io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
	//bit mask of the pins
	//io_conf.pin_bit_mask = GPIO_READY_LED_MASK;
	//set as output mode
	//io_conf.mode = GPIO_MODE_OUTPUT;
	//disable pull-up mode
	//io_conf.pull_up_en = 0;
	//io_conf.pull_down_en = 0;
	//gpio_config(&io_conf);
	
	//install gpio isr service
	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
}


// ************************************************
static void chipInfo(void){
	/* Print chip information */
	esp_chip_info_t chip_info;
	esp_chip_info(&chip_info);
	printf("This is ESP32 chip with %d CPU cores, WiFi%s%s, ",
			chip_info.cores,
			(chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
					(chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

	printf("silicon revision %d, ", chip_info.revision);

	printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
			(chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}


/*********************************************************
 *
 * Start task for deleting NVS wifi data
 *
 * *******************************************************/
void init_delete_button(void){

	vSemaphoreCreateBinary(delete_button_sem);
	//semaphore is available at the beginning, take it here
	xSemaphoreTake(delete_button_sem, 0);

	init_delete_button_io();

	if (delete_button_sem != NULL){
		xTaskCreate(&delete_button_fun, "delete_button_task",
					configMINIMAL_STACK_SIZE * 4, NULL, 0, NULL);
	}

}


/* ************************************************************
 *
 * delete button interrupt
 *
 * ***********************************************************/
static void IRAM_ATTR delete_button_isr_handler(void* arg){
	static portBASE_TYPE xHigherPriorityTaskWoken;

	if (nvs_delete_button_ready == true){
		xSemaphoreGiveFromISR(delete_button_sem, &xHigherPriorityTaskWoken);
		portYIELD_FROM_ISR();
		nvs_delete_button_ready = false;
	}
}


/*******************************************************************
 *
 * initialize delete button
 * this button deletes wifi parameters stored in NVS memory
 *
 * ******************************************************************/
void init_delete_button_io(void){
	gpio_config_t io_conf;

	//interrupt on both edges
	io_conf.intr_type = GPIO_INTR_NEGEDGE;
	//bit mask of the pins, use GPIO4/5 here
	io_conf.pin_bit_mask = GPIO_DELETE_BUTTON_MASK;
	//set as input mode
	io_conf.mode = GPIO_MODE_INPUT;
	//enable pull-up mode
	io_conf.pull_up_en = 1;
	io_conf.pull_down_en = 0;
	gpio_config(&io_conf);

	gpio_isr_handler_add(GPIO_DELETE_BUTTON, delete_button_isr_handler, NULL);
}


static void init_sntp(void)
{
    printf("Initializing SNTP\n");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

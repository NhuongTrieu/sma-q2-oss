#include <stdint.h>
#include <string.h>
#include "nordic_common.h"
#include "nrf.h"
#include "app_error.h"
#include "sma-q2.h"
#include "ble_init.h"
#include "ble_hci.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"
#include "softdevice_handler.h"
#include "app_timer.h"
#include "app_button.h"
#include "ble_nus.h"
#include "app_uart.h"
#include "app_util_platform.h"
#include "nrf_drv_wdt.h"
#include "nrf_drv_gpiote.h"
#include "hardware/buttons.h"
#include "nrf_gfx.h"
#include "lcd.h"
#include "app_time.h"
#include "watchface.h"
#include "battery.h"
#include "backlight.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"
#include "queue.h"
#include "tetris.h"
#include "applet.h"
#include "vibration.h"
#include "accel.h"
#include "pah8series_api_c.h"
#include "pah8002.h"
#include "app_hrm.h"
#include "app_music.h"
#include "nrf_rtc.h"
#include "hrm.h"

#define UART_TX_BUF_SIZE                256                                         /**< UART TX buffer size. */
#define UART_RX_BUF_SIZE                256                                         /**< UART RX buffer size. */

static const nrf_gfx_font_desc_t * p_font = &orkney_8ptFontInfo;
static const nrf_lcd_t * p_lcd = &nrf_lcd_lpm013m126a;

enum applet_id{
	APPLET_WATCHFACE,
	APPLET_TETRIS,
	APPLET_HRM,
	APPLET_MUSIC,
	APPLET_CALL,
	APPLET_NOTIFICATION
};

applet_t applets[]={
		{APPLET_WATCHFACE,draw_watchface,watchface_handle_button_evt},
		{APPLET_TETRIS,draw_tetris,tetris_handle_button_evt},
		{APPLET_HRM,draw_hrm,hrm_handle_button_evt},
		{APPLET_MUSIC,music_draw,music_handle_button_evt}

};

applet_t *current_applet=&applets[0];

uint8_t start_hrm=0;

volatile wchar_t test[100];

uint32_t get_stats_timer( void ){
	return nrf_rtc_counter_get(portNRF_RTC_REG);
}

TimerHandle_t lcd_com_timer = NULL;

void lcd_com_timer_callback( TimerHandle_t pxTimer ){

    nrf_gpio_pin_toggle(LCD_COM_PIN);

}

TimerHandle_t battery_timer = NULL;

void battery_timer_callback( TimerHandle_t pxTimer ){

    battery_sample();

}

void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info){

//	app_error_print(id, pc, info);
//	printf("fault\r\n");

	lcd_clear(RED);

	nrf_gfx_point_t text_start = NRF_GFX_POINT(5, 50);
	char error[20];
	snprintf(error,20,"F:%X,%X,%X",(unsigned int)id,(unsigned int)pc,(unsigned int)info);
    nrf_gfx_print(p_lcd, &text_start, WHITE, error, p_font, true);

    text_start.y+=10;
    snprintf(error,20,"E:0x%X", (unsigned int)((error_info_t *)(info))->err_code);
    nrf_gfx_print(p_lcd, &text_start, WHITE, error, p_font, true);

    nrf_gfx_display(p_lcd);
	while(1);

}



/**@brief   Function for handling app_uart events.
 *
 * @details This function will receive a single character from the app_uart module and append it to 
 *          a string. The string will be be sent over BLE when the last character received was a 
 *          'new line' i.e '\n' (hex 0x0D) or if the string has reached a length of 
 *          @ref NUS_MAX_DATA_LENGTH.
 */
/**@snippet [Handling the data received over UART] */
void uart_event_handle(app_uart_evt_t * p_event)
{
    static uint8_t data_array[BLE_NUS_MAX_DATA_LEN];
    static uint8_t index = 0;
//    uint32_t       err_code;


    switch (p_event->evt_type)
    {
        case APP_UART_DATA_READY:
            UNUSED_VARIABLE(app_uart_get(&data_array[index]));
            index++;

            if ((data_array[index - 1] == '\n') || (index >= (BLE_NUS_MAX_DATA_LEN)))
            {
//                err_code = ble_nus_string_send(&m_nus, data_array, index);
//                if (err_code != NRF_ERROR_INVALID_STATE)
//                {
//                    APP_ERROR_CHECK(err_code);
//                }
                
                index = 0;
            }
            break;

        case APP_UART_COMMUNICATION_ERROR:
//            APP_ERROR_HANDLER(p_event->data.error_communication);
            break;

        case APP_UART_FIFO_ERROR:
//            APP_ERROR_HANDLER(p_event->data.error_code);
            break;

        default:
            break;
    }
}
/**@snippet [Handling the data received over UART] */


/**@brief  Function for initializing the UART module.
 */
/**@snippet [UART Initialization] */
static void uart_init(void)
{
    uint32_t                     err_code;
    const app_uart_comm_params_t comm_params =
    {
        RX_PIN_NUMBER,
        TX_PIN_NUMBER,
        RTS_PIN_NUMBER,
        CTS_PIN_NUMBER,
        APP_UART_FLOW_CONTROL_DISABLED,
        false,
        UART_BAUDRATE_BAUDRATE_Baud115200
    };

    APP_UART_FIFO_INIT( &comm_params,
                       UART_RX_BUF_SIZE,
                       UART_TX_BUF_SIZE,
                       uart_event_handle,
                       APP_IRQ_PRIORITY_LOW,
                       err_code);
    APP_ERROR_CHECK(err_code);
}
/**@snippet [UART Initialization] */

char stats_buffer[250];

static TaskHandle_t  button_task;
static TaskHandle_t  time_task;
static TaskHandle_t  watchface_task;
static TaskHandle_t  vibration_task;
static TaskHandle_t  hrm_task;

static void watchface_task_handler(void * arg) {

	TickType_t xLastWakeTime;
	TickType_t delay = 200;

	lcd_com_timer = xTimerCreate("lcd_com", 1000, pdTRUE, 0,
			lcd_com_timer_callback);
	xTimerStart(lcd_com_timer, portMAX_DELAY);
	battery_timer = xTimerCreate("battery", 60000, pdTRUE, 0,
			battery_timer_callback);
	xTimerStart(battery_timer, portMAX_DELAY);
	battery_sample();

	uint8_t random[1];
	vTaskDelay(500);
	sd_rand_application_vector_get(random, 1);
	srand48(random[0]);
	button_event_t evt;

	xLastWakeTime = xTaskGetTickCount();

	for (;;) {
		nrf_wdt_reload_request_set(NRF_WDT_RR0);

		if (xQueueReceive(button_evt_queue, &evt, (TickType_t) 0)) {
			backlight_on();

			if (evt.press_type == LONG_PRESS) {
				vibration_short();
			}

			if (current_applet->id == APPLET_WATCHFACE) {

				if (evt.button == BUTTON_UP && evt.press_type == LONG_PRESS) {
//				vTaskGetRunTimeStats(stats_buffer);
//				puts(stats_buffer);
					delay = 200;
					current_applet = &applets[APPLET_MUSIC];
				}

				if (evt.button == BUTTON_DOWN && evt.press_type == LONG_PRESS) {
					delay = 100;
					current_applet = &applets[APPLET_TETRIS];
				}

				if (evt.button == BUTTON_OK && evt.press_type == LONG_PRESS) {
					xTaskNotify(hrm_task, HRM_START, eSetValueWithOverwrite);
					delay = 100;
					current_applet = &applets[APPLET_HRM];
				}

			}

			if (evt.button == BUTTON_BACK && evt.press_type == LONG_PRESS) {
				xTaskNotify(hrm_task, HRM_STOP, eSetValueWithOverwrite);
				delay = 200;
				current_applet = &applets[APPLET_WATCHFACE];
			}

			current_applet->handle_button_evt(&evt);
		}

//		lcd_clear(WHITE);
		current_applet->draw_applet();
		if (current_applet->id == APPLET_WATCHFACE) {
			draw_statusbar(0);
		} else {
			draw_statusbar(1);
//			backlight_on();
		}
		nrf_gfx_display(p_lcd);

//		vTaskDelay(100);
		vTaskDelayUntil(&xLastWakeTime, delay);
	}

}

static void button_task_handler(void * arg) {

	int pin_state,old_state=BUTTONS_MASK,pins_changed;

	int pin_counters[BUTTON_NUM];

    button_evt_queue = xQueueCreate( 10, sizeof( button_event_t ) );

	accel_init();

	for (;;) {

		pin_state=nrf_gpio_pins_read()&BUTTONS_MASK;

		pins_changed=pin_state^old_state;

		for (int i=0;i<BUTTON_NUM;i++){

			if ((pin_state>>button_pins[i])&1){

				if (pin_counters[i]>=BUTTON_LONG_PRESS_THRESHOLD){
									send_button_evt(i,LONG_PRESS_RELEASE,pin_counters[i]);
							}
				else if (pin_counters[i]>0){
					send_button_evt(i,SHORT_PRESS_RELEASE,0);
				}

				pin_counters[i]=0;
			}
			else{

				if (pin_counters[i]==0){
					send_button_evt(i,SHORT_PRESS,0);
				}
				else if (pin_counters[i]==BUTTON_LONG_PRESS_THRESHOLD){
					send_button_evt(i,LONG_PRESS,pin_counters[i]);
				}
				else if (pin_counters[i]>BUTTON_LONG_PRESS_THRESHOLD&&((pin_counters[i]%10)==0)){
					send_button_evt(i,LONG_PRESS_HOLDING,pin_counters[i]);
				}

				pin_counters[i]++;
			}

		}

		old_state=pin_state;

		 if (accel_check()){
			 backlight_on();
		 }


		vTaskDelay(50);

	}
}



static void time_task_handler(void * arg){


	for(;;){
		time_handler(xTaskGetTickCount());
		vTaskDelay(250);
	}

}



static void vibration_task_handler(void * arg){

	for(;;){
		vibration_step();
		vTaskDelay(1000/VIBRATION_PATTERN_LEN);
	}

}



static void hrm_task_handler(void * arg) {

	uint32_t command = 0;
	pah8002_init(hrm_task);

	for (;;) {
		pah8002_task();
		if (xTaskNotifyWait(0x00, UINT32_MAX, &command, 50) == pdTRUE) {

			switch (command) {
			case HRM_START:
				hrm_start();
				break;
			case HRM_STOP:
				hrm_stop();
				break;
			default:
				break;
			}
		}

//		ulTaskNotifyTake( pdTRUE, portMAX_DELAY );
//		vTaskDelay(50);
	}

}

#define FPU_EXCEPTION_MASK 0x0000009F

void FPU_IRQHandler(void)
{
    uint32_t *fpscr = (uint32_t *)(FPU->FPCAR+0x40);
    (void)__get_FPSCR();

    *fpscr = *fpscr & ~(FPU_EXCEPTION_MASK);
}

/**@brief Application main function.
 */
int main(void)
{
    uint32_t err_code;

    // Initialize.
    APP_TIMER_INIT(APP_TIMER_PRESCALER, APP_TIMER_OP_QUEUE_SIZE, false);

    nrf_drv_gpiote_init();
    buttons_init();

    nrf_gfx_init(p_lcd);

    lcd_clear(CYAN);
    nrf_gfx_point_t text_start = NRF_GFX_POINT(5, 10);
    nrf_gfx_print(p_lcd, &text_start, WHITE, "SMA-Q2-OSS", p_font, true);
    text_start.y +=p_font->height;
    nrf_gfx_print(p_lcd, &text_start, BLUE, "Blue", p_font, true);
    text_start.y +=p_font->height;
    nrf_gfx_print(p_lcd, &text_start, YELLOW, "Yellow", p_font, true);
    text_start.y +=p_font->height;
    nrf_gfx_print(p_lcd, &text_start, PINK, "Pink", p_font, true);
    nrf_gfx_display(p_lcd);

//    uart_init();
    battery_init();
    backlight_init();
    vibration_init();
//    for (;;){
//    __WFE();
//    }

//    printf("\r\nUART Start!\r\n");
    ble_stack_init();
    gap_params_init();
    services_init();
    advertising_init();
    conn_params_init();

    err_code = ble_advertising_start(BLE_ADV_MODE_SLOW);
    APP_ERROR_CHECK(err_code);
    
//    for (;;){
//    __WFE();
//    }

    xTaskCreate(watchface_task_handler, "watchface", 256, NULL, 1, &watchface_task);
    xTaskCreate(button_task_handler, "buttons", 128, NULL, 1, &button_task);
    xTaskCreate(time_task_handler, "time", 128, NULL, 1, &time_task);
    xTaskCreate(vibration_task_handler, "vibration", 128, NULL, 1, &vibration_task);
    xTaskCreate(hrm_task_handler, "hrm", 256, NULL, 1, &hrm_task);

    vTaskStartScheduler();

    for (;;)
    {
    }
}


/** 
 * @}
 */

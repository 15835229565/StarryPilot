/*
 * File      : logger.c
 *
 *
 * Change Logs:
 * Date           Author       	Notes
 * 2018-03-29     zoujiachi   	the first version
 */

#include "logger.h"
#include "global.h"
#include "ff.h"
#include "file_manager.h"
#include "console.h"
#include "delay.h"
#include "uMCN.h"
#include "motor.h"
#include "adrc.h"
#include "adrc_att.h"
#include "att_estimator.h"
#include "global.h"
#include <string.h>
#include <stdlib.h>

#define LOGGER_DEFAULT_PERIOD		100
#define EVENT_LOG_RECORD			(1<<0)

LOG_HeaderDef* log_header_t = NULL;

static char* TAG = "Logger";
FIL logger_fp;
LOGGER_InfoDef _logger_info;
static struct rt_timer _timer_logger;
static struct rt_event event_log;

MCN_DECLARE(ATT_EULER);
MCN_DECLARE(SENSOR_GYR);
MCN_DECLARE(SENSOR_FILTER_GYR);
MCN_DECLARE(SENSOR_ACC);
MCN_DECLARE(SENSOR_FILTER_ACC);
MCN_DECLARE(SENSOR_MAG);
MCN_DECLARE(SENSOR_FILTER_MAG);
MCN_DECLARE(MOTOR_THROTTLE);
MCN_DECLARE(ADRC);

LOG_ElementInfoDef element_info_list[] =
{
	LOG_ELEMENT_INFO_FLOAT(ROLL),
	LOG_ELEMENT_INFO_FLOAT(PITCH),
	LOG_ELEMENT_INFO_FLOAT(YAW),
	LOG_ELEMENT_INFO_FLOAT(GYR_X),
	LOG_ELEMENT_INFO_FLOAT(GYR_Y),
	LOG_ELEMENT_INFO_FLOAT(GYR_Z),
	LOG_ELEMENT_INFO_FLOAT(GYR_FILTER_X),
	LOG_ELEMENT_INFO_FLOAT(GYR_FILTER_Y),
	LOG_ELEMENT_INFO_FLOAT(GYR_FILTER_Z),
	LOG_ELEMENT_INFO_FLOAT(ACC_X),
	LOG_ELEMENT_INFO_FLOAT(ACC_Y),
	LOG_ELEMENT_INFO_FLOAT(ACC_Z),
	LOG_ELEMENT_INFO_FLOAT(ACC_FILTER_X),
	LOG_ELEMENT_INFO_FLOAT(ACC_FILTER_Y),
	LOG_ELEMENT_INFO_FLOAT(ACC_FILTER_Z),
	LOG_ELEMENT_INFO_FLOAT(MAG_X),
	LOG_ELEMENT_INFO_FLOAT(MAG_Y),
	LOG_ELEMENT_INFO_FLOAT(MAG_Z),
	LOG_ELEMENT_INFO_FLOAT(MAG_FILTER_X),
	LOG_ELEMENT_INFO_FLOAT(MAG_FILTER_Y),
	LOG_ELEMENT_INFO_FLOAT(MAG_FILTER_Z),
	LOG_ELEMENT_INFO_FLOAT(MOTOR_1),
	LOG_ELEMENT_INFO_FLOAT(MOTOR_2),
	LOG_ELEMENT_INFO_FLOAT(MOTOR_3),
	LOG_ELEMENT_INFO_FLOAT(MOTOR_4),
	LOG_ELEMENT_INFO_FLOAT(ADRC_PITCH_SP_RATE),
	LOG_ELEMENT_INFO_FLOAT(ADRC_PITCH_V),
	LOG_ELEMENT_INFO_FLOAT(ADRC_PITCH_V1),
	LOG_ELEMENT_INFO_FLOAT(ADRC_PITCH_V2),
	LOG_ELEMENT_INFO_FLOAT(ADRC_PITCH_Z1),
	LOG_ELEMENT_INFO_FLOAT(ADRC_PITCH_Z2),
};

uint8_t logger_create_header(uint32_t log_period)
{
	log_header_t = (LOG_HeaderDef*)rt_malloc(sizeof(LOG_HeaderDef));
	if(log_header_t == NULL){
		Console.e(TAG, "err, can not create log header\n");
		return 1;
	}
	
	log_header_t->start_time = time_nowMs();
	log_header_t->log_period = log_period;
	log_header_t->element_num = sizeof(element_info_list)/sizeof(LOG_ElementInfoDef);
	log_header_t->header_size = sizeof(LOG_HeaderDef) - sizeof(LOG_ElementInfoDef*) + sizeof(element_info_list);
	log_header_t->field_size = sizeof(LOG_FieldDef);
	if(log_header_t->element_num > LOG_MAX_ELEMENT_NUM){
		Console.print("log element num is larger than maximal element num\n");
		rt_free(log_header_t);
		return 2;
	}
	
	log_header_t->element_info = (LOG_ElementInfoDef*)rt_malloc(log_header_t->element_num*sizeof(LOG_ElementInfoDef));
	if(log_header_t->element_info == NULL){
		Console.e(TAG, "err, fail to malloc for element_info\n");
		rt_free(log_header_t);
		return 1;
	}
	for(int i = 0 ; i < log_header_t->element_num ; i++)
		log_header_t->element_info[i] = element_info_list[i];
	
	return 0;
}

void logger_release_header(void)
{
	rt_free(log_header_t->element_info);
	rt_free(log_header_t);
	log_header_t = NULL;
}

uint8_t logger_start(char* file_name, uint32_t log_period)
{
	uint8_t res = 0;
	if(!fm_init_complete()){
		Console.e(TAG, "err, file system is not init properly\n");
		return 1;
	}
	
	if(_logger_info.status == LOGGER_BUSY){
		Console.print("logger is busy, please first stop log\n");
		return 2;
	}
	
	if(logger_create_header(log_period>0 ? log_period : LOGGER_DEFAULT_PERIOD))
		return 3;
	
	/* create log file */
	UINT bw;
	FRESULT fres = f_open(&logger_fp, file_name, FA_OPEN_ALWAYS | FA_WRITE);
	if(fres == FR_OK){
		fres = f_write(&logger_fp, log_header_t, sizeof(LOG_HeaderDef)-sizeof(LOG_ElementInfoDef*), &bw);
		if(fres == FR_OK && bw == sizeof(LOG_HeaderDef)-sizeof(LOG_ElementInfoDef*)){
			fres = f_write(&logger_fp, log_header_t->element_info, log_header_t->element_num*sizeof(LOG_ElementInfoDef), &bw);
			if(fres != FR_OK || bw!= log_header_t->element_num*sizeof(LOG_ElementInfoDef)){
				Console.e(TAG, "log header write fail:%d bw:%d\n", fres, bw);
				res = 4;
				goto error;
			}
			
			rt_tick_t tick = log_period>0 ? log_period : LOGGER_DEFAULT_PERIOD;
			_logger_info.status = LOGGER_BUSY;
			_logger_info.last_record_time = 0;
			_logger_info.log_period = tick;
			
			/* start logger timer */
			rt_timer_control(&_timer_logger, RT_TIMER_CTRL_SET_TIME, &tick);
			rt_timer_start(&_timer_logger);
			
			Console.print("log file create successful, start to log... tick=%d\n", tick);
		}else{
			Console.e(TAG, "log header write fail:%d bw:%d\n", fres, bw);
			res = 4;
		}
	}else{
		Console.e(TAG, "log file create fail:%d\n", fres);
		res = 4;
	}
	
error:	
	logger_release_header();
	
	return res;
}

void logger_stop(void)
{
	rt_timer_stop(&_timer_logger);
	f_close(&logger_fp);
	_logger_info.status = LOGGER_IDLE;
	Console.print("logger stop successful\n");
}

uint8_t logger_record(void)
{
	float gyr[3], acc[3], filter_gyr[3], filter_acc[3], mag[3], filter_mag[3];
	float throttle[MOTOR_NUM];
	Euler euler;
	ADRC_Log adrc_log;
	mcn_copy_from_hub(MCN_ID(ATT_EULER), &euler);
	mcn_copy_from_hub(MCN_ID(SENSOR_GYR), gyr);
	mcn_copy_from_hub(MCN_ID(SENSOR_FILTER_GYR), filter_gyr);
	mcn_copy_from_hub(MCN_ID(SENSOR_ACC), acc);
	mcn_copy_from_hub(MCN_ID(SENSOR_FILTER_ACC), filter_acc);
	mcn_copy_from_hub(MCN_ID(SENSOR_MAG), mag);
	mcn_copy_from_hub(MCN_ID(SENSOR_FILTER_MAG), filter_mag);
	mcn_copy_from_hub(MCN_ID(MOTOR_THROTTLE), throttle);
	mcn_copy_from_hub(MCN_ID(ADRC), &adrc_log);
	
	LOG_SET_ELEMENT(_logger_info, ROLL, Rad2Deg(euler.roll));
	LOG_SET_ELEMENT(_logger_info, PITCH, Rad2Deg(euler.pitch));
	LOG_SET_ELEMENT(_logger_info, YAW, Rad2Deg(euler.yaw));
	LOG_SET_ELEMENT(_logger_info, GYR_X, gyr[0]);
	LOG_SET_ELEMENT(_logger_info, GYR_Y, gyr[1]);
	LOG_SET_ELEMENT(_logger_info, GYR_Z, gyr[2]);
	LOG_SET_ELEMENT(_logger_info, GYR_FILTER_X, filter_gyr[0]);
	LOG_SET_ELEMENT(_logger_info, GYR_FILTER_Y, filter_gyr[1]);
	LOG_SET_ELEMENT(_logger_info, GYR_FILTER_Z, filter_gyr[2]);
	LOG_SET_ELEMENT(_logger_info, ACC_X, acc[0]);
	LOG_SET_ELEMENT(_logger_info, ACC_Y, acc[1]);
	LOG_SET_ELEMENT(_logger_info, ACC_Z, acc[2]);
	LOG_SET_ELEMENT(_logger_info, ACC_FILTER_X, filter_acc[0]);
	LOG_SET_ELEMENT(_logger_info, ACC_FILTER_Y, filter_acc[1]);
	LOG_SET_ELEMENT(_logger_info, ACC_FILTER_Z, filter_acc[2]);
	LOG_SET_ELEMENT(_logger_info, MAG_X, mag[0]);
	LOG_SET_ELEMENT(_logger_info, MAG_Y, mag[1]);
	LOG_SET_ELEMENT(_logger_info, MAG_Z, mag[2]);
	LOG_SET_ELEMENT(_logger_info, MAG_FILTER_X, filter_mag[0]);
	LOG_SET_ELEMENT(_logger_info, MAG_FILTER_Y, filter_mag[1]);
	LOG_SET_ELEMENT(_logger_info, MAG_FILTER_Z, filter_mag[2]);
	LOG_SET_ELEMENT(_logger_info, MOTOR_1, throttle[0]);
	LOG_SET_ELEMENT(_logger_info, MOTOR_2, throttle[1]);
	LOG_SET_ELEMENT(_logger_info, MOTOR_3, throttle[2]);
	LOG_SET_ELEMENT(_logger_info, MOTOR_4, throttle[3]);
	LOG_SET_ELEMENT(_logger_info, ADRC_PITCH_SP_RATE, adrc_log.sp_rate);
	LOG_SET_ELEMENT(_logger_info, ADRC_PITCH_V, adrc_log.v);
	LOG_SET_ELEMENT(_logger_info, ADRC_PITCH_V1, adrc_log.v1);
	LOG_SET_ELEMENT(_logger_info, ADRC_PITCH_V2, adrc_log.v2);
	LOG_SET_ELEMENT(_logger_info, ADRC_PITCH_Z1, adrc_log.z1);
	LOG_SET_ELEMENT(_logger_info, ADRC_PITCH_Z2, adrc_log.z2);
	
	UINT bw;
	FRESULT res = f_write(&logger_fp, &_logger_info.log_field, sizeof(_logger_info.log_field), &bw);
	
	return (bw == sizeof(_logger_info.log_field)) ? 0 : 1;
}

void logger_show_element_info(uint32_t element_num, const LOG_ElementInfoDef* element_info)
{
	Console.print("%-20s %-10s\n", "Name", "Type");
	for(uint32_t i = 0 ; i < element_num ; i++){
		char ele_type[10];
		switch(element_info[i].type)
		{
			case LOG_INT8:
			{
				strcpy(ele_type, "INT8");
			}break;
			case LOG_UINT8:
			{
				strcpy(ele_type, "UINT8");
			}break;
			case LOG_INT16:
			{
				strcpy(ele_type, "INT16");
			}break;
			case LOG_UINT16:
			{
				strcpy(ele_type, "UINT16");
			}break;
			case LOG_INT32:
			{
				strcpy(ele_type, "INT32");
			}break;
			case LOG_UINT32:
			{
				strcpy(ele_type, "UINT32");
			}break;
			case LOG_FLOAT:
			{
				strcpy(ele_type, "FLOAT");
			}break;
			case LOG_DOUBLE:
			{
				strcpy(ele_type, "DOUBLE");
			}break;
		}
		Console.print("%-20s %-10s\n", element_info[i].name, ele_type);
	}
}

uint8_t logger_parse_header(char* file_name)
{
	FIL fp;
	UINT br;
	
	if(log_header_t != NULL){
		Console.print("logger is busy now\n");
		return 1;
	}
	log_header_t = (LOG_HeaderDef*)rt_malloc(sizeof(LOG_HeaderDef));
	if(log_header_t == NULL){
		return 1;
	}
	
	FRESULT fres = f_open(&fp, file_name, FA_OPEN_EXISTING | FA_READ);
	if(fres != FR_OK){
		Console.print("%s open fail!\n", file_name);
		return 2;
	}
	
	fres = f_read(&fp, &log_header_t->start_time, sizeof(log_header_t->start_time), &br);
	fres = f_read(&fp, &log_header_t->log_period, sizeof(log_header_t->log_period), &br);
	fres = f_read(&fp, &log_header_t->element_num, sizeof(log_header_t->element_num), &br);
	fres = f_read(&fp, &log_header_t->header_size, sizeof(log_header_t->header_size), &br);
	fres = f_read(&fp, &log_header_t->field_size, sizeof(log_header_t->field_size), &br);
	log_header_t->element_info = (LOG_ElementInfoDef*)rt_malloc(log_header_t->element_num*sizeof(LOG_ElementInfoDef));
	if(log_header_t->element_info == NULL){
		logger_release_header();
		return 0;
	}
	f_read(&fp, log_header_t->element_info, log_header_t->element_num*sizeof(LOG_ElementInfoDef), &br);
	f_close(&fp);
	
	Console.print("Start Time: %d\n", log_header_t->start_time);
	Console.print("Log Period: %d\n", log_header_t->log_period);
	Console.print("Element Number: %d\n", log_header_t->element_num);
	Console.print("Header Size: %d byte\n", log_header_t->header_size);
	Console.print("Field Size: %d byte\n", log_header_t->field_size);
	logger_show_element_info(log_header_t->element_num, log_header_t->element_info);
	
	logger_release_header();
	return 0;
}

int handle_logger_shell_cmd(int argc, char** argv)
{
	int res = 0;
	
	if(argc > 1){
		if(strcmp(argv[1], "start") == 0){
			if(argc == 3)
				res = logger_start(argv[2], 0);	// default period
			if(argc == 4)
				res = logger_start(argv[2], atoi(argv[3]));
		}
		if(strcmp(argv[1], "stop") == 0){
			logger_stop();
		}
		if(strcmp(argv[1], "info") == 0 && argc == 3){
			res = logger_parse_header(argv[2]);
		}
	}
	
	return res;
}

static void timer_logger_record(void* parameter)
{
	rt_event_send(&event_log, EVENT_LOG_RECORD);
}

void logger_entry(void *parameter)
{
	rt_err_t res;
	rt_uint32_t recv_set = 0;
	rt_uint32_t wait_set = EVENT_LOG_RECORD;
	
	/* create event */
	res = rt_event_init(&event_log, "logger_event", RT_IPC_FLAG_FIFO);
	
	rt_timer_init(&_timer_logger, "logger",
					timer_logger_record,
					RT_NULL,
					LOGGER_DEFAULT_PERIOD,
					RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
	
	while(1)
	{
		/* wait event occur */
		res = rt_event_recv(&event_log, wait_set, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 
								RT_WAITING_FOREVER, &recv_set);
		
		if(res == RT_EOK){
			logger_record();
		}else{
			/* some error happens */
			Console.e(TAG, "logger loop, err:%d\r\n" , res);
		}
	}
}


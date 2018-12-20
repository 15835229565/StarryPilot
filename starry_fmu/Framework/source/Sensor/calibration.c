/**
* File      : calibration.c
*
* Least-squre ellipsoid fitting
*/
/*****************************************************************************
Copyright (c) 2018, StarryPilot Development Team. All rights reserved.

Author: Jiachi Zou, weety

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of StarryPilot nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

#include <rtdevice.h>
#include <rtthread.h>
#include <string.h>
#include <math.h>
#include "console.h"
#include "sensor_manager.h"
#include "systime.h"
#include "shell.h"
#include "calibration.h"
#include "light_matrix.h"
#include "uMCN.h"
#include "mavproxy.h"
#include "mavlink_param.h"
#include "param.h"
#include "msh_usr_cmd.h"
#include "ff.h"

#define GYR_DEFAULT_NUM			2000
#define GYR_DEFAULT_PERIOD		5

#define ACC_DEFAULT_NUM			100
#define ACC_DEFAULT_PERIOD		5

#define MAG_DEFAULT_NUM			1500
#define MAG_DEFAULT_PERIOD		20

static bool gyr_calibrate_flag;

char _acc_instruction[6][20] = {
	"put z-axis down.",
	"put z-axis up.",
	"put y-axis down.",
	"put y-axis up.",
	"put x-axis down.",
	"put x-axis up.",
};

MCN_DECLARE(SENSOR_MEASURE_GYR);
MCN_DECLARE(SENSOR_MEASURE_ACC);
MCN_DECLARE(SENSOR_MEASURE_MAG);

#define MS_TO_TICKS(ms) (ms * RT_TICK_PER_SECOND / 1000)

typedef struct {
	bool jitter;
	float last_gyr[3];
	uint16_t count;
	uint16_t jitter_counter;
} copter_jitter_t;

copter_jitter_t jitter = {0};

#define ACC_MAX_THRESHOLD 9.3f
#define ACC_MIN_THRESHOLD 0.6f

typedef enum {
	ACC_POS_FRONT,
	ACC_POS_BACK,
	ACC_POS_LEFT,
	ACC_POS_RIGHT,
	ACC_POS_UP,
	ACC_POS_DOWN,
} acc_position;

typedef struct {
	int acc_calibrate_flag;
	union {
		struct {
			unsigned int front_flag: 1;
			unsigned int back_flag: 1;
			unsigned int left_flag: 1;
			unsigned int right_flag: 1;
			unsigned int up_flag: 1;
			unsigned int down_flag: 1;
			unsigned int step: 3;
			unsigned int obj_flag: 1;
		} bit;
		unsigned int val;
	} pos;
	int sample_cnt;
	int sample_flag;
	int detect_cnt;
	acc_position cur_pos;
	Cali_Obj obj;
} acc_t;

static acc_t acc = {0};

typedef struct {
	uint32_t last_time;
	bool mag_calibrate_flag;
	float rotation_angle;
	union {
		struct {
			unsigned int down_flag: 1;
			unsigned int front_flag: 1;
			unsigned int obj_flag: 1;
			unsigned int step: 3;
		} bit;
		unsigned int val;
	} stat;
	Cali_Obj obj;
} mag_t;

static mag_t mag = {0};

void gyr_mavlink_calibration(void)
{
	float gyr_data_p[3];
	static float sum_gyr[3] = {0.0f, 0.0f, 0.0f};
	float offset_gyr[3];
	static uint16_t count = 0;
	int ret = 0;

	if(!gyr_calibrate_flag) {
		return;
	}

	sensor_gyr_measure(gyr_data_p);
	sum_gyr[0] += gyr_data_p[0];
	sum_gyr[1] += gyr_data_p[1];
	sum_gyr[2] += gyr_data_p[2];
	count++;

	if(!(count % 20) || (count == GYR_CALIBRATE_COUNT)) {
		mavlink_send_calibration_progress_msg(((float)count / GYR_CALIBRATE_COUNT) * 10);
	}

	if(count == GYR_CALIBRATE_COUNT) {
		offset_gyr[0] = sum_gyr[0] / count;
		offset_gyr[1] = sum_gyr[1] / count;
		offset_gyr[2] = sum_gyr[2] / count;
		sum_gyr[0] = 0.0f;
		sum_gyr[1] = 0.0f;
		sum_gyr[2] = 0.0f;

		ret = mavlink_param_set_value_by_index(CAL_GYRO0_XOFF, offset_gyr[0]);
		ret |= mavlink_param_set_value_by_index(CAL_GYRO0_YOFF, offset_gyr[1]);
		ret |= mavlink_param_set_value_by_index(CAL_GYRO0_ZOFF, offset_gyr[2]);

		if(!ret) {
			PARAM_SET_UINT32(CALIBRATION, GYR_CALIB, 1);
			mavlink_send_status(CAL_DONE);
		} else {
			mavlink_send_status(CAL_FAILED);
		}

		count = 0;
		gyr_calibrate_flag = false;
	}
}

void gyr_mavlink_calibration_start(void)
{
	gyr_calibrate_flag = true;
}

void cali_obj_init(Cali_Obj* obj, uint8_t rotated_fit)
{
	for(int i = 0 ; i < 9 ; i++) {
		obj->V[i] = 0.0f;
		obj->D[i] = 0.0f;

		for(int j = 0 ; j < 9 ; j++) {
			obj->P[i][j] = 0.0f;
		}
	}

	obj->P[0][0] = obj->P[1][1] = obj->P[2][2] = 10.0f;

	if(rotated_fit) {
		obj->P[3][3] = obj->P[4][4] = obj->P[5][5] = 1.0f;
	} else {
		obj->P[3][3] = obj->P[4][4] = obj->P[5][5] = 0.0f;
	}

	obj->P[6][6] = obj->P[7][7] = obj->P[8][8] = 1.0f;
	obj->R = 0.001f;

	MatCreate(&obj->EigVec, 3, 3);
	MatCreate(&obj->RotM, 3, 3);
}

void cali_obj_delete(Cali_Obj* obj)
{
	MatDelete(&obj->EigVec);
	MatDelete(&obj->RotM);
}

void cali_least_squre_update(Cali_Obj* obj, float val[3])
{
	double x = val[0];
	double y = val[1];
	double z = val[2];

	obj->D[0] = x * x;
	obj->D[1] = y * y;
	obj->D[2] = z * z;
	obj->D[3] = 2.0f * x * y;
	obj->D[4] = 2.0f * x * z;
	obj->D[5] = 2.0f * y * z;
	obj->D[6] = 2.0f * x;
	obj->D[7] = 2.0f * y;
	obj->D[8] = 2.0f * z;

	// Y = Z-D*V
	float DV = 0.0f;

	for(uint8_t i = 0 ; i < 9 ; i++) {
		DV += obj->D[i] * obj->V[i];
	}

	float Y = 1.0f - DV;

	// S = D*P*D' + R
	double DP[9];

	for(uint8_t i = 0 ; i < 9 ; i++) {
		DP[i] = 0.0f;

		for(uint8_t j = 0 ; j < 9 ; j++) {
			DP[i] += obj->D[j] * obj->P[j][i];
		}
	}

	double DPDT = 0.0f;

	for(uint8_t i = 0 ; i < 9 ; i++) {
		DPDT += DP[i] * obj->D[i];
	}

	double S = DPDT + obj->R;

	// K = P*D'/S
	double K[9];

	for(uint8_t i = 0 ; i < 9 ; i++) {
		K[i] = 0.0f;

		for(uint8_t j = 0 ; j < 9 ; j++) {
			K[i] += obj->P[i][j] * obj->D[j];
		}

		K[i] = K[i] / S;
	}

	// V = V + K*Y
	for(uint8_t i = 0 ; i < 9 ; i++) {
		obj->V[i] += K[i] * Y;
	}

	// P = P - K*D*P
	double KD[9][9];

	for(uint8_t i = 0 ; i < 9 ; i++) {
		for(uint8_t j = 0 ; j < 9 ; j++) {
			KD[i][j] = K[i] * obj->D[j];
		}
	}

	double KDP[9][9];

	for(uint8_t i = 0 ; i < 9 ; i++) {
		for(uint8_t j = 0 ; j < 9 ; j++) {
			KDP[i][j] = 0.0f;

			for(uint8_t k = 0 ; k < 9 ; k++) {
				KDP[i][j] += KD[i][k] * obj->P[j][k];
			}
		}
	}

	for(uint8_t i = 0 ; i < 9 ; i++) {
		for(uint8_t j = 0 ; j < 9 ; j++) {
			obj->P[i][j] -= KDP[i][j];
		}
	}
}

void cali_solve(Cali_Obj* obj, double radius)
{
	Mat A, B;
	Mat InvB;
	Mat Tmtx;
	Mat AT;
	Mat TmtxA, TmtxTrans;
	Mat E;
	Mat GMat;
	Mat InvEigVec;

	MatCreate(&A, 4, 4);
	MatCreate(&B, 3, 3);
	MatCreate(&InvB, 3, 3);
	MatCreate(&Tmtx, 4, 4);
	MatCreate(&AT, 4, 4);
	MatCreate(&TmtxA, 4, 4);
	MatCreate(&TmtxTrans, 4, 4);
	MatCreate(&E, 3, 3);
	MatCreate(&GMat, 3, 3);
	MatCreate(&InvEigVec, 3, 3);

	LIGHT_MATRIX_TYPE valA[16] = {
		obj->V[0], obj->V[3], obj->V[4], obj->V[6],
		obj->V[3], obj->V[1], obj->V[5], obj->V[7],
		obj->V[4], obj->V[5], obj->V[2], obj->V[8],
		obj->V[6], obj->V[7], obj->V[8],    -1
	};
	MatSetVal(&A, valA);

	LIGHT_MATRIX_TYPE valB[9] = {
		obj->V[0], obj->V[3], obj->V[4],
		obj->V[3], obj->V[1], obj->V[5],
		obj->V[4], obj->V[5], obj->V[2],
	};
	MatSetVal(&B, valB);

	MatInv(&B, &InvB);

	LIGHT_MATRIX_TYPE v1[3] = {obj->V[6], obj->V[7], obj->V[8]};

	for(uint8_t i = 0 ; i < 3 ; i++) {
		obj->OFS[i] = 0.0f;

		for(uint8_t j = 0 ; j < 3 ; j++) {
			obj->OFS[i] += InvB.element[i][j] * v1[j];
		}

		obj->OFS[i] = -obj->OFS[i];
	}

	MatEye(&Tmtx);
	Tmtx.element[3][0] = obj->OFS[0];
	Tmtx.element[3][1] = obj->OFS[1];
	Tmtx.element[3][2] = obj->OFS[2];

	MatMul(&Tmtx, &A, &TmtxA);
	MatTrans(&Tmtx, &TmtxTrans);
	MatMul(&TmtxA, &TmtxTrans, &AT);

	LIGHT_MATRIX_TYPE valE[9] = {
		-AT.element[0][0] / AT.element[3][3], -AT.element[0][1] / AT.element[3][3], -AT.element[0][2] / AT.element[3][3],
		    -AT.element[1][0] / AT.element[3][3], -AT.element[1][1] / AT.element[3][3], -AT.element[1][2] / AT.element[3][3],
		    -AT.element[2][0] / AT.element[3][3], -AT.element[2][1] / AT.element[3][3], -AT.element[2][2] / AT.element[3][3]
	    };
	MatSetVal(&E, valE);

	LIGHT_MATRIX_TYPE eig_val[3];
	MatEig(&E, eig_val, &obj->EigVec, 1e-6, 100);

	for(uint8_t i = 0 ; i < 3 ; i++) {
		obj->GAIN[i] = sqrt(1.0 / eig_val[i]);
	}

	/* calculate transform matrix */
	MatZeros(&GMat);
	GMat.element[0][0] = 1.0 / obj->GAIN[0] * radius;
	GMat.element[1][1] = 1.0 / obj->GAIN[1] * radius;
	GMat.element[2][2] = 1.0 / obj->GAIN[2] * radius;

	MatInv(&obj->EigVec, &InvEigVec);

	Mat tmp;
	MatCreate(&tmp, 3, 3);
	MatMul(&obj->EigVec, &GMat, &tmp);
	MatMul(&tmp, &InvEigVec, &obj->RotM);

	MatDelete(&A);
	MatDelete(&B);
	MatDelete(&InvB);
	MatDelete(&Tmtx);
	MatDelete(&AT);
	MatDelete(&TmtxA);
	MatDelete(&TmtxTrans);
	MatDelete(&E);
	MatDelete(&GMat);
	MatDelete(&InvEigVec);
	MatDelete(&tmp);
}

static void copter_jitter_check(void)
{
	float gyr_data_p[3];
	float offset_gyr[3];
	int ret = 0;

	if(jitter.count < 20) {
		jitter.count++;
		sensor_gyr_measure(gyr_data_p);
		offset_gyr[0] = gyr_data_p[0] - jitter.last_gyr[0];
		offset_gyr[1] = gyr_data_p[1] - jitter.last_gyr[1];
		offset_gyr[2] = gyr_data_p[2] - jitter.last_gyr[2];
		jitter.last_gyr[0] = gyr_data_p[0];
		jitter.last_gyr[1] = gyr_data_p[1];
		jitter.last_gyr[2] = gyr_data_p[2];

		if(fabsf(offset_gyr[0]) > 0.8 || fabsf(offset_gyr[1]) > 0.8 || fabsf(offset_gyr[2]) > 0.8) {
			jitter.jitter_counter++;
		}
	} else {
		if(jitter.jitter_counter > 10) {
			jitter.jitter = true;
		} else {
			jitter.jitter = false;
		}

		jitter.count = 0;
		jitter.jitter_counter = 0;
	}
}

static void acc_position_detect(void)
{
	float acc_f[3];
	sensor_acc_measure(acc_f);
	// mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_ACC), acc_f);

	if((fabsf(acc_f[0]) > ACC_MAX_THRESHOLD) &&
	        (fabsf(acc_f[1]) < ACC_MIN_THRESHOLD) &&
	        (fabsf(acc_f[2]) < ACC_MIN_THRESHOLD)) {
		if(acc_f[0] < 0) {
			acc.cur_pos = ACC_POS_FRONT;
		} else {
			acc.cur_pos = ACC_POS_BACK;
		}
	}

	if((fabsf(acc_f[0]) < ACC_MIN_THRESHOLD) &&
	        (fabsf(acc_f[1]) > ACC_MAX_THRESHOLD) &&
	        (fabsf(acc_f[2]) < ACC_MIN_THRESHOLD)) {
		if(acc_f[1] < 0) {
			acc.cur_pos = ACC_POS_RIGHT;
		} else {
			acc.cur_pos = ACC_POS_LEFT;
		}
	}

	if((fabsf(acc_f[0]) < ACC_MIN_THRESHOLD) &&
	        (fabsf(acc_f[1]) < ACC_MIN_THRESHOLD) &&
	        (fabsf(acc_f[2]) > ACC_MAX_THRESHOLD)) {
		if(acc_f[2] > 0) {
			acc.cur_pos = ACC_POS_UP;
		} else {
			acc.cur_pos = ACC_POS_DOWN;
		}
	}
}

void acc_mavlink_calibration(void)
{
	float acc_f[3];

	if(!acc.acc_calibrate_flag) {
		return;
	}

	if(!acc.pos.bit.obj_flag) {
		cali_obj_init(&(acc.obj), 0);
		acc.pos.bit.obj_flag = 1;
	}

	copter_jitter_check();
	acc_position_detect();

	if((acc.cur_pos == ACC_POS_FRONT) && !acc.pos.bit.front_flag) {
		if(!jitter.jitter) {
			acc.detect_cnt++;
		} else {
			acc.detect_cnt = 0;
		}

		if(acc.detect_cnt > ACC_POS_DETECT_COUNT) {
			acc.sample_flag = 1;
			acc.pos.bit.front_flag = 1;
			acc.sample_cnt = 0;
			acc.pos.bit.step++;
			mavlink_send_status(CAL_FRONT_DETECTED);
		}
	}

	if((acc.cur_pos == ACC_POS_BACK) && !acc.pos.bit.back_flag) {
		if(!jitter.jitter) {
			acc.detect_cnt++;
		} else {
			acc.detect_cnt = 0;
		}

		if(acc.detect_cnt > ACC_POS_DETECT_COUNT) {
			acc.sample_flag = 1;
			acc.pos.bit.back_flag = 1;
			acc.sample_cnt = 0;
			acc.pos.bit.step++;
			mavlink_send_status(CAL_BACK_DETECTED);
		}
	}

	if((acc.cur_pos == ACC_POS_LEFT) && !acc.pos.bit.left_flag) {
		if(!jitter.jitter) {
			acc.detect_cnt++;
		} else {
			acc.detect_cnt = 0;
		}

		if(acc.detect_cnt > ACC_POS_DETECT_COUNT) {
			acc.sample_flag = 1;
			acc.pos.bit.left_flag = 1;
			acc.sample_cnt = 0;
			acc.pos.bit.step++;
			mavlink_send_status(CAL_LEFT_DETECTED);
		}
	}

	if((acc.cur_pos == ACC_POS_RIGHT) && !acc.pos.bit.right_flag) {
		if(!jitter.jitter) {
			acc.detect_cnt++;
		} else {
			acc.detect_cnt = 0;
		}

		if(acc.detect_cnt > ACC_POS_DETECT_COUNT) {
			acc.sample_flag = 1;
			acc.pos.bit.right_flag = 1;
			acc.sample_cnt = 0;
			acc.pos.bit.step++;
			mavlink_send_status(CAL_RIGHT_DETECTED);
		}
	}

	if((acc.cur_pos == ACC_POS_UP) && !acc.pos.bit.up_flag) {
		if(!jitter.jitter) {
			acc.detect_cnt++;
		} else {
			acc.detect_cnt = 0;
		}

		if(acc.detect_cnt > ACC_POS_DETECT_COUNT) {
			acc.sample_flag = 1;
			acc.pos.bit.up_flag = 1;
			acc.sample_cnt = 0;
			acc.pos.bit.step++;
			mavlink_send_status(CAL_UP_DETECTED);
		}
	}

	if((acc.cur_pos == ACC_POS_DOWN) && !acc.pos.bit.down_flag) {
		if(!jitter.jitter) {
			acc.detect_cnt++;
		} else {
			acc.detect_cnt = 0;
		}

		if(acc.detect_cnt > ACC_POS_DETECT_COUNT) {
			acc.sample_flag = 1;
			acc.pos.bit.down_flag = 1;
			acc.sample_cnt = 0;
			acc.pos.bit.step++;
			mavlink_send_status(CAL_DOWN_DETECTED);
		}
	}

	if(acc.sample_flag) {
		if(acc.sample_cnt < ACC_SAMPLE_COUNT) {
			sensor_acc_measure(acc_f);
			// mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_ACC), acc_f);
			cali_least_squre_update(&(acc.obj), acc_f);
			acc.sample_cnt++;
		} else if(acc.sample_cnt == ACC_SAMPLE_COUNT) {
			acc.detect_cnt = 0;
			acc.sample_cnt++;
			acc.sample_flag = 0;

			switch(acc.cur_pos) {
				case ACC_POS_FRONT:
					mavlink_send_status(CAL_FRONT_DONE);
					break;

				case ACC_POS_BACK:
					mavlink_send_status(CAL_BACK_DONE);
					break;

				case ACC_POS_LEFT:
					mavlink_send_status(CAL_LEFT_DONE);
					break;

				case ACC_POS_RIGHT:
					mavlink_send_status(CAL_RIGHT_DONE);
					break;

				case ACC_POS_UP:
					mavlink_send_status(CAL_UP_DONE);
					break;

				case ACC_POS_DOWN:
					mavlink_send_status(CAL_DOWN_DONE);
					break;

				default:
					break;
			}

			mavlink_send_calibration_progress_msg(((float)acc.pos.bit.step / 6) * 10);
		}
	}

	if((acc.pos.bit.step == 6) && (acc.sample_cnt > ACC_SAMPLE_COUNT)) {
		cali_solve(&(acc.obj), GRAVITY_MSS);
		Console.print("Center:%f %f %f\n", acc.obj.OFS[0], acc.obj.OFS[1], acc.obj.OFS[2]);
		Console.print("Radius:%f %f %f\n", acc.obj.GAIN[0], acc.obj.GAIN[1], acc.obj.GAIN[2]);
		Console.print("Rotation Matrix:\n");

		for(int row = 0 ; row < acc.obj.RotM.row ; row++) {
			for(int col = 0 ; col < acc.obj.RotM.col ; col++) {
				Console.print("%.4f\t", acc.obj.RotM.element[row][col]);
			}

			Console.print("\n");
		}

		PARAM_SET_FLOAT(CALIBRATION, ACC_BIAS_X, acc.obj.OFS[0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_BIAS_Y, acc.obj.OFS[1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_BIAS_Z, acc.obj.OFS[2]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_1, acc.obj.RotM.element[0][0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_2, acc.obj.RotM.element[0][1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_3, acc.obj.RotM.element[0][2]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_4, acc.obj.RotM.element[1][0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_5, acc.obj.RotM.element[1][1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_6, acc.obj.RotM.element[1][2]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_7, acc.obj.RotM.element[2][0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_8, acc.obj.RotM.element[2][1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_9, acc.obj.RotM.element[2][2]);
		PARAM_SET_UINT32(CALIBRATION, ACC_CALIB, 1);

		param_store();

		mavlink_send_status(CAL_DONE);
		cali_obj_delete(&(acc.obj));
		acc.pos.bit.obj_flag = 0;
		acc.acc_calibrate_flag = false;
		acc.pos.val = 0;
		acc.sample_cnt = 0;

	}
}

void acc_mavlink_calibration_start(void)
{
	acc.acc_calibrate_flag = true;
}

void mag_mavlink_calibration(void)
{
	float mag_f[3];
	float gyr_f[3];
	float delta_t;
	uint32_t now;

	if(!mag.mag_calibrate_flag) {
		return;
	}

	sensor_gyr_measure(gyr_f);
	// mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_GYR), gyr_f);
	now = time_nowMs();
	delta_t = (now - mag.last_time) * 1e-3;
	mag.last_time = now;

	acc_position_detect();

	switch(mag.stat.bit.step) {
		case 0: {
			if(!mag.stat.bit.obj_flag) {
				cali_obj_init(&(mag.obj), 1);
				mag.stat.bit.obj_flag = 1;
			}

			if((acc.cur_pos == ACC_POS_UP) || (acc.cur_pos == ACC_POS_DOWN)) {
				mag.stat.bit.step = 1;
			}

			break;
		}

		case 1: {
			mag.rotation_angle += gyr_f[2] * delta_t;

			if(!mag.stat.bit.down_flag) {
				mavlink_send_status(CAL_DOWN_DETECTED);
				mag.stat.bit.down_flag = 1;
			}

			sensor_mag_measure(mag_f);
			// mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_MAG), mag_f);
			cali_least_squre_update(&(mag.obj), mag_f);
			mavlink_send_calibration_progress_msg(fabsf(mag.rotation_angle) / (2 * PI / 5));

			if(fabsf(mag.rotation_angle) > (2 * PI)) {
				mag.stat.bit.step = 2;
				mag.rotation_angle = 0;
				mavlink_send_status(CAL_DOWN_DONE);
			}

			break;
		}

		case 2: {
			if((acc.cur_pos == ACC_POS_FRONT) || (acc.cur_pos == ACC_POS_BACK)) {
				mag.stat.bit.step = 3;

				if(!mag.stat.bit.front_flag) {
					mavlink_send_status(CAL_FRONT_DETECTED);
					mag.stat.bit.front_flag = 1;
				}
			}

			break;
		}

		case 3: {
			mag.rotation_angle += gyr_f[0] * delta_t;

			sensor_mag_measure(mag_f);
			// mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_MAG), mag_f);
			cali_least_squre_update(&(mag.obj), mag_f);
			mavlink_send_calibration_progress_msg(fabsf(mag.rotation_angle + 2 * PI) / (2 * PI / 5));

			if(fabsf(mag.rotation_angle) > (2 * PI)) {
				mag.stat.bit.step = 4;
				mag.rotation_angle = 0;
				mavlink_send_status(CAL_FRONT_DONE);
			}

			break;
		}

		case 4: {
			cali_solve(&(mag.obj), 1);

			Console.print("Center:%f %f %f\n", mag.obj.OFS[0], mag.obj.OFS[1], mag.obj.OFS[2]);
			Console.print("Radius:%f %f %f\n", mag.obj.GAIN[0], mag.obj.GAIN[1], mag.obj.GAIN[2]);
			Console.print("Rotation Matrix:\n");

			for(int row = 0 ; row < mag.obj.RotM.row ; row++) {
				for(int col = 0 ; col < mag.obj.RotM.col ; col++) {
					Console.print("%.4f\t", mag.obj.RotM.element[row][col]);
				}

				Console.print("\n");
			}

			PARAM_SET_FLOAT(CALIBRATION, MAG_BIAS_X, mag.obj.OFS[0]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_BIAS_Y, mag.obj.OFS[1]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_BIAS_Z, mag.obj.OFS[2]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_1, mag.obj.RotM.element[0][0]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_2, mag.obj.RotM.element[0][1]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_3, mag.obj.RotM.element[0][2]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_4, mag.obj.RotM.element[1][0]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_5, mag.obj.RotM.element[1][1]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_6, mag.obj.RotM.element[1][2]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_7, mag.obj.RotM.element[2][0]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_8, mag.obj.RotM.element[2][1]);
			PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_9, mag.obj.RotM.element[2][2]);
			PARAM_SET_UINT32(CALIBRATION, MAG_CALIB, 1);

			param_store();
			mavlink_send_status(CAL_DONE);
			cali_obj_delete(&(mag.obj));
			mag.stat.bit.obj_flag = 0;
			mag.mag_calibrate_flag = false;
			mag.stat.val = 0;
			break;
		}

		default:
			break;
	}

}

void mag_mavlink_calibration_start(void)
{
	mag.mag_calibrate_flag = true;
}

int calibrate_acc_run(uint32_t num, uint32_t period, bool echo, FIL* fid, bool rotated_fitting)
{
	Cali_Obj obj;
	float acc_f[3];

	cali_obj_init(&obj, rotated_fitting);

	for(int n = 0 ; n < 6 ; n++) {
		Console.print("%s {Y/N}\n", _acc_instruction[n]);
		char ch = shell_wait_ch();

		if(ch == 'Y' || ch == 'y') {

			Console.print("reading data...\n");

			for(int i = 0 ; i < num ; i ++) {
				//mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_ACC), acc_f);
				sensor_acc_measure(acc_f);
				cali_least_squre_update(&obj, acc_f);

				if(echo)
					Console.print("%lf %lf %lf\n", acc_f[0], acc_f[1], acc_f[2]);

				if(fid != NULL) {
					UINT bw;
					char str_buffer[50];

					sprintf(str_buffer, "%lf %lf %lf\n", acc_f[0], acc_f[1], acc_f[2]);
					f_write(fid, str_buffer, strlen(str_buffer), &bw);
				}

				rt_thread_delay(period);
			}
		} else {
			goto finish;
		}
	}

	cali_solve(&obj, GRAVITY_MSS);
	Console.print("center:%f %f %f\n", obj.OFS[0], obj.OFS[1], obj.OFS[2]);
	Console.print("radius:%f %f %f\n", obj.GAIN[0], obj.GAIN[1], obj.GAIN[2]);
	Console.print("rotation matrix:\n");

	for(int row = 0 ; row < obj.RotM.row ; row++) {
		for(int col = 0 ; col < obj.RotM.col ; col++) {
			Console.print("%.4f\t", obj.RotM.element[row][col]);
		}

		Console.print("\n");
	}

	Console.print("store to parameter? (Y/N)\n");
	char ch = shell_wait_ch();

	if(ch == 'Y' || ch == 'y') {
		PARAM_SET_FLOAT(CALIBRATION, ACC_BIAS_X, obj.OFS[0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_BIAS_Y, obj.OFS[1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_BIAS_Z, obj.OFS[2]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_1, obj.RotM.element[0][0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_2, obj.RotM.element[0][1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_3, obj.RotM.element[0][2]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_4, obj.RotM.element[1][0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_5, obj.RotM.element[1][1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_6, obj.RotM.element[1][2]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_7, obj.RotM.element[2][0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_8, obj.RotM.element[2][1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_ROT_MAT_9, obj.RotM.element[2][2]);
		PARAM_SET_UINT32(CALIBRATION, ACC_CALIB, 1);

		param_store();
	}

finish:
	cali_obj_delete(&obj);
	return 0;
}

int calibrate_mag_run(uint32_t num, uint32_t period, bool echo, FIL* fid, bool rotated_fitting)
{
	Cali_Obj obj;
	float mag_f[3];

	cali_obj_init(&obj, rotated_fitting);

	Console.print("rotate with each axis... (Y/N)\n");
	char ch = shell_wait_ch();

	if(ch == 'Y' || ch == 'y') {
		Console.print("reading data...\n");

		for(int i = 0 ; i < num ; i ++) {
			//mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_MAG), mag_f);
			sensor_mag_measure(mag_f);
			cali_least_squre_update(&obj, mag_f);

			if(echo)
				Console.print("%lf %lf %lf\n", mag_f[0], mag_f[1], mag_f[2]);

			if(fid != NULL) {
				UINT bw;
				char str_buffer[50];

				sprintf(str_buffer, "%lf %lf %lf\n", mag_f[0], mag_f[1], mag_f[2]);
				f_write(fid, str_buffer, strlen(str_buffer), &bw);
			}

			rt_thread_delay(period);
		}
	} else {
		goto finish;
	}

	// solve
	cali_solve(&obj, 1);

	Console.print("center:%f %f %f\n", obj.OFS[0], obj.OFS[1], obj.OFS[2]);
	Console.print("radius:%f %f %f\n", obj.GAIN[0], obj.GAIN[1], obj.GAIN[2]);
	Console.print("rotation matrix:\n");

	for(int row = 0 ; row < obj.RotM.row ; row++) {
		for(int col = 0 ; col < obj.RotM.col ; col++) {
			Console.print("%.4f\t", obj.RotM.element[row][col]);
		}

		Console.print("\n");
	}

	Console.print("store to parameter? (Y/N)\n");
	ch = shell_wait_ch();

	if(ch == 'Y' || ch == 'y') {
		PARAM_SET_FLOAT(CALIBRATION, MAG_BIAS_X, obj.OFS[0]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_BIAS_Y, obj.OFS[1]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_BIAS_Z, obj.OFS[2]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_1, obj.RotM.element[0][0]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_2, obj.RotM.element[0][1]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_3, obj.RotM.element[0][2]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_4, obj.RotM.element[1][0]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_5, obj.RotM.element[1][1]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_6, obj.RotM.element[1][2]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_7, obj.RotM.element[2][0]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_8, obj.RotM.element[2][1]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_ROT_MAT_9, obj.RotM.element[2][2]);
		PARAM_SET_UINT32(CALIBRATION, MAG_CALIB, 1);

		param_store();
	}

finish:
	cali_obj_delete(&obj);
	return 0;
}

int calibrate_gyr_run(uint32_t num, uint32_t period, bool echo, FIL* fid)
{
	float gyr_f[3];
	double sum_gyr[3] = {0.0f, 0.0f, 0.0f};
	float offset_gyr[3];

	Console.print("start to calibrate gyr\n");
	Console.print("keep the board static...(Y/N)\n");
	char ch = shell_wait_ch();

	if(ch == 'Y' || ch == 'y') {
		Console.print("reading data...\n");

		for(uint32_t i = 0 ; i < num ; i++) {
			sensor_gyr_measure(gyr_f);
			sum_gyr[0] += gyr_f[0];
			sum_gyr[1] += gyr_f[1];
			sum_gyr[2] += gyr_f[2];

			if(echo)
				Console.print("%lf %lf %lf\n", gyr_f[0], gyr_f[1], gyr_f[2]);

			if(fid != NULL) {
				UINT bw;
				char str_buffer[50];

				sprintf(str_buffer, "%lf %lf %lf\n", gyr_f[0], gyr_f[1], gyr_f[2]);
				f_write(fid, str_buffer, strlen(str_buffer), &bw);
			}

			rt_thread_delay(period);
		}
	} else {
		return 1;
	}

	offset_gyr[0] = sum_gyr[0] / num;
	offset_gyr[1] = sum_gyr[1] / num;
	offset_gyr[2] = sum_gyr[2] / num;

	Console.print("gyr offset:%f %f %f\r\n\n", offset_gyr[0], offset_gyr[1], offset_gyr[2]);

	Console.print("store to parameter? (Y/N)\n");
	ch = shell_wait_ch();

	if(ch == 'Y' || ch == 'y') {
		PARAM_SET_FLOAT(CALIBRATION, GYR_BIAS_X, offset_gyr[0]);
		PARAM_SET_FLOAT(CALIBRATION, GYR_BIAS_Y, offset_gyr[1]);
		PARAM_SET_FLOAT(CALIBRATION, GYR_BIAS_Z, offset_gyr[2]);
		PARAM_SET_UINT32(CALIBRATION, GYR_CALIB, 1);

		param_store();
	}

	return 0;
}

int handle_calibrate_shell_cmd(int argc, char** argv, int optc, sh_optv* optv)
{
	int res = 0;
	uint8_t sensor_type = 0;
	uint32_t num, period;
	bool echo = false;
	bool rotated_fitting = false;
	bool save_file = false;
	FIL fid;

	if(argc > 1) {
		if(strcmp("gyr", argv[1]) == 0) {
			sensor_type = 1;

			num = GYR_DEFAULT_NUM;
			period = GYR_DEFAULT_PERIOD;
		}

		if(strcmp("acc", argv[1]) == 0) {
			sensor_type = 2;

			num = ACC_DEFAULT_NUM;
			period = ACC_DEFAULT_PERIOD;
			rotated_fitting = false;
		}

		if(strcmp("mag", argv[1]) == 0) {
			sensor_type = 3;

			num = MAG_DEFAULT_NUM;
			period = MAG_DEFAULT_PERIOD;
			rotated_fitting = true;
		}
	}

	// handle option
	for(uint16_t i = 0 ; i < optc ; i++) {

		if(strcmp(optv[i].opt, "--echo") == 0 || strcmp(optv[i].opt, "-e") == 0) {

			echo = true;
		}

		if(strcmp(optv[i].opt, "--period") == 0 || strcmp(optv[i].opt, "-p") == 0) {

			if(!shell_is_number(optv[i].val))
				continue;

			period = atoi(optv[i].val);
		}

		if(strcmp(optv[i].opt, "--num") == 0 || strcmp(optv[i].opt, "-n") == 0) {

			if(!shell_is_number(optv[i].val))
				continue;

			num = atoi(optv[i].val);
		}

		if(strcmp(optv[i].opt, "--save_file") == 0) {

			if(optv[i].val == NULL)
				continue;

			FRESULT fres = f_open(&fid, optv[i].val, FA_OPEN_ALWAYS | FA_WRITE);

			if(fres == FR_OK) {
				save_file = true;
			} else {
				Console.print("%s open fail:%d\n", optv[i].val, fres);
				save_file = false;
			}
		}

		if(strcmp(optv[i].opt, "--rotated_fitting") == 0 || strcmp(optv[i].opt, "-r") == 0) {

			if(optv[i].val == NULL)
				continue;

			if(strcmp(optv[i].val, "true") == 0) {
				rotated_fitting = true;
			} else {
				rotated_fitting = false;
			}
		}
	}

	FIL* fid_t = save_file == true ? &fid : NULL;

	switch(sensor_type) {
		case 1: {
			res = calibrate_gyr_run(num, period, echo, fid_t);
		}
		break;

		case 2: {
			res = calibrate_acc_run(num, period, echo, fid_t, rotated_fitting);
		}
		break;

		case 3: {
			res = calibrate_mag_run(num, period, echo, fid_t, rotated_fitting);
		}
		break;

		default:
			break;
	}

	if(fid_t) {
		f_close(fid_t);
	}

	return res;
}

void rt_cali_thread_entry(void* parameter)
{

	while(1) {
		gyr_mavlink_calibration();
		acc_mavlink_calibration();
		mag_mavlink_calibration();
		rt_thread_sleep(MS_TO_TICKS(CALI_THREAD_SLEEP_MS));
	}
}


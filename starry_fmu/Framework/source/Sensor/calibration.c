/**
* File      : calibration.c
*
* 最小二乘法椭球拟合校正算法
*
/*****************************************************************************
Copyright (c) 2018, StarryPilot Development Team. All rights reserved.

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
#include "delay.h"
#include "shell.h"
#include "calibration.h"
#include "light_matrix.h"
#include "uMCN.h"
#include "mavproxy.h"
#include "mavlink_param.h"
#include "param.h"

static bool gyr_calibrate_flag;

MCN_DECLARE(SENSOR_MEASURE_GYR);
MCN_DECLARE(SENSOR_MEASURE_ACC);
MCN_DECLARE(SENSOR_MEASURE_MAG);

void gyr_mavlink_calibration(void)
{
	float gyr_data_p[3];
	static float sum_gyr[3] = {0.0f,0.0f,0.0f};
	float offset_gyr[3];
	static uint16_t count = 0;
	int ret = 0;

	if (!gyr_calibrate_flag) {
		return;
	}

	sensor_gyr_measure(gyr_data_p);
	sum_gyr[0] += gyr_data_p[0];
	sum_gyr[1] += gyr_data_p[1];
	sum_gyr[2] += gyr_data_p[2];
	count++;

	if (!(count % 20) || (count == GYR_CALIBRATE_COUNT)) {
		mavlink_send_calibration_progress_msg(((float)count / GYR_CALIBRATE_COUNT) * 10);
	}

	if (count == GYR_CALIBRATE_COUNT) {
		offset_gyr[0] = -sum_gyr[0]/count;
		offset_gyr[1] = -sum_gyr[1]/count;
		offset_gyr[2] = -sum_gyr[2]/count;
		sum_gyr[0] = 0.0f;
		sum_gyr[1] = 0.0f;
		sum_gyr[2] = 0.0f;

		ret = mavlink_param_set_value_by_index(CAL_GYRO0_XOFF, offset_gyr[0]);
		ret |= mavlink_param_set_value_by_index(CAL_GYRO0_YOFF, offset_gyr[1]);
		ret |= mavlink_param_set_value_by_index(CAL_GYRO0_ZOFF, offset_gyr[2]);

		if (!ret) {
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

void cali_obj_init(Cali_Obj *obj)
{
	for(int i = 0 ; i < 9 ; i++){
		obj->V[i] = 0.0f;
		obj->D[i] = 0.0f;
		for(int j = 0 ; j < 9 ; j++){
			obj->P[i][j] = 0.0f;
		}
	}
	
	obj->P[0][0] = obj->P[1][1] = obj->P[2][2] = 10.0f;
	obj->P[3][3] = obj->P[4][4] = obj->P[5][5] = 1.0f;
	obj->P[6][6] = obj->P[7][7] = obj->P[8][8] = 1.0f;
	obj->R = 0.001f;
	
	MatCreate(&obj->EigVec, 3, 3);
	MatCreate(&obj->RotM, 3, 3);
}

void cali_obj_delete(Cali_Obj *obj)
{
	MatDelete(&obj->EigVec);
	MatDelete(&obj->RotM);
}

void cali_least_squre_update(Cali_Obj *obj, float val[3])
{
	double x = val[0];
	double y = val[1];
	double z = val[2];
	
	obj->D[0] = x*x;
	obj->D[1] = y*y;
	obj->D[2] = z*z;
	obj->D[3] = 2.0f*x*y;
	obj->D[4] = 2.0f*x*z;
	obj->D[5] = 2.0f*y*z;
	obj->D[6] = 2.0f*x;
	obj->D[7] = 2.0f*y;
	obj->D[8] = 2.0f*z;
	
	// Y = Z-D*V
	float DV = 0.0f;
	for(uint8_t i = 0 ; i < 9 ; i++){
		DV += obj->D[i]*obj->V[i];
	}
	float Y = 1.0f - DV;
	
	// S = D*P*D' + R
	double DP[9];
	for(uint8_t i = 0 ; i < 9 ; i++){
		DP[i] = 0.0f;
		for(uint8_t j = 0 ; j < 9 ; j++){
			DP[i] += obj->D[j]*obj->P[j][i];
		}
	}
	double DPDT = 0.0f;
	for(uint8_t i = 0 ; i < 9 ; i++){
		DPDT += DP[i] * obj->D[i];
	}
	double S = DPDT + obj->R;
	
	// K = P*D'/S
	double K[9];
	for(uint8_t i = 0 ; i < 9 ; i++){
		K[i] = 0.0f;
		for(uint8_t j = 0 ; j < 9 ; j++){
			K[i] += obj->P[i][j] * obj->D[j];
		}
		K[i] = K[i]/S;
	}
	
	// V = V + K*Y
	for(uint8_t i = 0 ; i < 9 ; i++){
		obj->V[i] += K[i]*Y;
	}
	
	// P = P - K*D*P
	double KD[9][9];
	for(uint8_t i = 0 ; i < 9 ; i++){
		for(uint8_t j = 0 ; j < 9 ; j++){
			KD[i][j] = K[i] * obj->D[j];
		}
	}
	double KDP[9][9];
	for(uint8_t i = 0 ; i < 9 ; i++){
		for(uint8_t j = 0 ; j < 9 ; j++){
			KDP[i][j] = 0.0f;
			for(uint8_t k = 0 ; k < 9 ; k++){
				KDP[i][j] += KD[i][k] * obj->P[j][k];
			}
		}
	}	
	for(uint8_t i = 0 ; i < 9 ; i++){
		for(uint8_t j = 0 ; j < 9 ; j++){
			obj->P[i][j] -= KDP[i][j];
		}
	}	
}

void cali_solve(Cali_Obj *obj, double radius)
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
	for(uint8_t i = 0 ; i < 3 ; i++){
		obj->OFS[i] = 0.0f;
		for(uint8_t j = 0 ; j < 3 ; j++){
			obj->OFS[i] += InvB.element[i][j]*v1[j];
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
		-AT.element[0][0]/AT.element[3][3], -AT.element[0][1]/AT.element[3][3], -AT.element[0][2]/AT.element[3][3],
		-AT.element[1][0]/AT.element[3][3], -AT.element[1][1]/AT.element[3][3], -AT.element[1][2]/AT.element[3][3],
		-AT.element[2][0]/AT.element[3][3], -AT.element[2][1]/AT.element[3][3], -AT.element[2][2]/AT.element[3][3]
	};
	MatSetVal(&E, valE);
	
	LIGHT_MATRIX_TYPE eig_val[3];
	MatEig(&E, eig_val, &obj->EigVec, 1e-6, 100);
	
	for(uint8_t i = 0 ; i < 3 ; i++){
		obj->GAIN[i] = sqrt(1.0/eig_val[i]);
	}
	
	/* calculate transform matrix */
	MatZeros(&GMat);
	GMat.element[0][0] = 1.0/obj->GAIN[0]*radius;
	GMat.element[1][1] = 1.0/obj->GAIN[1]*radius;
	GMat.element[2][2] = 1.0/obj->GAIN[2]*radius;
	
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

/**************************** Calibrate method 2 End ************************************/

int calibrate_acc_run(void)
{
	Console.print("Start to calibrate acc\n");
	
	Cali_Obj obj;
	char ch;
	cali_obj_init(&obj);
	
	Console.print("towards Z-axis to DOWN side, and keep it static...{Y/N}\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		Console.print("reading data...\n");
		
		float acc_f[3];
		for(int i = 0 ; i < 100 ; i ++){
			mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_ACC), acc_f);
			cali_least_squre_update(&obj, acc_f);
			//Console.print("%lf %lf %lf\n", acc_f[0], acc_f[1], acc_f[2]);
			rt_thread_delay(20);
		}
	}else{
		goto finish;
	}
	
	Console.print("towards Z-axis to UP side, and keep it static...{Y/N}\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		Console.print("reading data...\n");
		
		float acc_f[3];
		for(int i = 0 ; i < 100 ; i ++){
			mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_ACC), acc_f);
			cali_least_squre_update(&obj, acc_f);
			//Console.print("%lf %lf %lf\n", acc_f[0], acc_f[1], acc_f[2]);
			rt_thread_delay(20);
		}
	}else{
		goto finish;
	}
	
	Console.print("towards X-axis to DOWN side, and keep it static...{Y/N}\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		Console.print("reading data...\n");
		
		float acc_f[3];
		for(int i = 0 ; i < 100 ; i ++){
			mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_ACC), acc_f);
			cali_least_squre_update(&obj, acc_f);
			//Console.print("%lf %lf %lf\n", acc_f[0], acc_f[1], acc_f[2]);
			rt_thread_delay(20);
		}
	}else{
		goto finish;
	}
	
	Console.print("towards X-axis to UP side, and keep it static...{Y/N}\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		Console.print("reading data...\n");
		
		float acc_f[3];
		for(int i = 0 ; i < 100 ; i ++){
			mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_ACC), acc_f);
			cali_least_squre_update(&obj, acc_f);
			//Console.print("%lf %lf %lf\n", acc_f[0], acc_f[1], acc_f[2]);
			rt_thread_delay(20);
		}
	}else{
		goto finish;
	}
	
	Console.print("towards Y-axis to DOWN side, and keep it static...{Y/N}\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		Console.print("reading data...\n");
		
		float acc_f[3];
		for(int i = 0 ; i < 100 ; i ++){
			mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_ACC), acc_f);
			cali_least_squre_update(&obj, acc_f);
			//Console.print("%lf %lf %lf\n", acc_f[0], acc_f[1], acc_f[2]);
			rt_thread_delay(20);
		}
	}else{
		goto finish;
	}
	
	Console.print("towards Y-axis to UP side, and keep it static...{Y/N}\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		Console.print("reading data...\n");
		
		float acc_f[3];
		for(int i = 0 ; i < 100 ; i ++){
			mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_ACC), acc_f);
			cali_least_squre_update(&obj, acc_f);
			//Console.print("%lf %lf %lf\n", acc_f[0], acc_f[1], acc_f[2]);
			rt_thread_delay(20);
		}
	}else{
		goto finish;
	}
	
	cali_solve(&obj, GRAVITY_MSS);
	Console.print("Center:%f %f %f\n", obj.OFS[0],obj.OFS[1],obj.OFS[2]);
	Console.print("Radius:%f %f %f\n", obj.GAIN[0],obj.GAIN[1],obj.GAIN[2]);
	Console.print("Rotation Matrix:\n");
	for(int row = 0 ; row < obj.RotM.row ; row++){
		for(int col = 0 ; col < obj.RotM.col ; col++){
			Console.print("%.4f\t", obj.RotM.element[row][col]);
		}
		Console.print("\n");
	}
	
	Console.print("store to parameter? (Y/N)\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		PARAM_SET_FLOAT(CALIBRATION, ACC_X_OFFSET, obj.OFS[0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_Y_OFFSET, obj.OFS[1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_Z_OFFSET, obj.OFS[2]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_TRANS_MAT00, obj.RotM.element[0][0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_TRANS_MAT01, obj.RotM.element[0][1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_TRANS_MAT02, obj.RotM.element[0][2]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_TRANS_MAT10, obj.RotM.element[1][0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_TRANS_MAT11, obj.RotM.element[1][1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_TRANS_MAT12, obj.RotM.element[1][2]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_TRANS_MAT20, obj.RotM.element[2][0]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_TRANS_MAT21, obj.RotM.element[2][1]);
		PARAM_SET_FLOAT(CALIBRATION, ACC_TRANS_MAT22, obj.RotM.element[2][2]);
		PARAM_SET_UINT32(CALIBRATION, ACC_CALIB, 1);
		
		param_store();
	}
	
finish:
	cali_obj_delete(&obj);
	return 0;
}

int calibrate_mag_run(void)
{
	Console.print("Start to calibrate mag\n");
	
	char ch;
	Cali_Obj obj;
	cali_obj_init(&obj);
	
	float mag_f[3];
	
	Console.print("towards Z-axis to DOWN side, and rotate with it...(Y/N)\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		Console.print("reading data...\n");
		for(int i = 0 ; i < 200 ; i ++){
			mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_MAG), mag_f);
			cali_least_squre_update(&obj, mag_f);
			//Console.print("%lf %lf %lf\n", mag_f[0], mag_f[1], mag_f[2]);
			rt_thread_delay(50);
		}	
	}else{
		goto finish;
	}
	
	Console.print("towards X-axis to DOWN side, and rotate with it...(Y/N)\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		Console.print("reading data...\n");
		for(int i = 0 ; i < 200 ; i ++){
			mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_MAG), mag_f);
			cali_least_squre_update(&obj, mag_f);
			//Console.print("%lf %lf %lf\n", mag_f[0], mag_f[1], mag_f[2]);
			rt_thread_delay(50);
		}	
	}else{
		goto finish;
	}
	
	Console.print("towards Y-axis to DOWN side, and rotate with it...(Y/N)\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		Console.print("reading data...\n");
		for(int i = 0 ; i < 200 ; i ++){
			mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_MAG), mag_f);
			cali_least_squre_update(&obj, mag_f);
			//Console.print("%lf %lf %lf\n", mag_f[0], mag_f[1], mag_f[2]);
			rt_thread_delay(50);
		}	
	}else{
		goto finish;
	}

	cali_solve(&obj, 1);
	
	Console.print("Center:%f %f %f\n", obj.OFS[0],obj.OFS[1],obj.OFS[2]);
	Console.print("Radius:%f %f %f\n", obj.GAIN[0],obj.GAIN[1],obj.GAIN[2]);
	Console.print("Rotation Matrix:\n");
	for(int row = 0 ; row < obj.RotM.row ; row++){
		for(int col = 0 ; col < obj.RotM.col ; col++){
			Console.print("%.4f\t", obj.RotM.element[row][col]);
		}
		Console.print("\n");
	}
	
	Console.print("store to parameter? (Y/N)\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		PARAM_SET_FLOAT(CALIBRATION, MAG_X_OFFSET, obj.OFS[0]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_Y_OFFSET, obj.OFS[1]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_Z_OFFSET, obj.OFS[2]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_TRANS_MAT00, obj.RotM.element[0][0]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_TRANS_MAT01, obj.RotM.element[0][1]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_TRANS_MAT02, obj.RotM.element[0][2]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_TRANS_MAT10, obj.RotM.element[1][0]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_TRANS_MAT11, obj.RotM.element[1][1]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_TRANS_MAT12, obj.RotM.element[1][2]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_TRANS_MAT20, obj.RotM.element[2][0]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_TRANS_MAT21, obj.RotM.element[2][1]);
		PARAM_SET_FLOAT(CALIBRATION, MAG_TRANS_MAT22, obj.RotM.element[2][2]);
		PARAM_SET_UINT32(CALIBRATION, MAG_CALIB, 1);
		
		param_store();
	}
	
finish:
	cali_obj_delete(&obj);
	return 0;
}

int calibrate_gyr_run(void)
{
	float gyr_f[3];
	double sum_gyr[3] = {0.0f,0.0f,0.0f};
	float offset_gyr[3];
	char ch;
	int p_num = 2000;
	
	Console.print("start to calibrate gyr\n");
	Console.print("keep the board static...(Y/N)\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		Console.print("reading data...\n");
		for(uint32_t i = 0 ; i < p_num ; i++)
		{
			mcn_copy_from_hub(MCN_ID(SENSOR_MEASURE_GYR), gyr_f);
			sum_gyr[0] += gyr_f[0];
			sum_gyr[1] += gyr_f[1];
			sum_gyr[2] += gyr_f[2];
			rt_thread_delay(5);
		}
	}else{
		return 1;
	}
	
	offset_gyr[0] = -sum_gyr[0]/p_num;
	offset_gyr[1] = -sum_gyr[1]/p_num;
	offset_gyr[2] = -sum_gyr[2]/p_num;
	
	Console.print("gyr offset:%f %f %f\r\n\n" , offset_gyr[0],offset_gyr[1],offset_gyr[2]);
	
	Console.print("store to parameter? (Y/N)\n");
	ch = shell_wait_ch();
	if(ch == 'Y' || ch == 'y'){
		PARAM_SET_FLOAT(CALIBRATION, GYR_X_OFFSET, offset_gyr[0]);
		PARAM_SET_FLOAT(CALIBRATION, GYR_Y_OFFSET, offset_gyr[1]);
		PARAM_SET_FLOAT(CALIBRATION, GYR_Z_OFFSET, offset_gyr[2]);
		PARAM_SET_UINT32(CALIBRATION, GYR_CALIB, 1);
		
		param_store();
	}
	return 0;
}

int handle_calib_shell_cmd(int argc, char** argv)
{
	int res = 0;
	if(argc > 1){
		if(strcmp("gyr", argv[1]) == 0){
			res = calibrate_gyr_run();
		}
		
		if(strcmp("acc", argv[1]) == 0){
			res = calibrate_acc_run();
		}
		
		if(strcmp("mag", argv[1]) == 0){
			res = calibrate_mag_run();
		}
	}
	
	return res;
}

/*
 * Academic License - for use in teaching, academic research, and meeting
 * course requirements at degree granting institutions only.  Not for
 * government, commercial, or other organizational use.
 *
 * File: EKF_GpsCorrect.c
 *
 * Code generated for Simulink model 'INS'.
 *
 * Model version                  : 1.141
 * Simulink Coder version         : 9.0 (R2018b) 24-May-2018
 * C/C++ source code generated on : Sun Oct 28 12:14:29 2018
 *
 * Target selection: ert.tlc
 * Embedded hardware selection: ARM Compatible->ARM Cortex
 * Code generation objectives: Unspecified
 * Validation result: Not run
 */

#include "EKF_GpsCorrect.h"

/* Include model header file for global data */
#include "INS.h"
#include "INS_private.h"

/* Output and update for Simulink Function: '<Root>/Simulink Function7' */
void EKF_GpsCorrect(const real32_T rtu_X[14], real32_T rty_y[2])
{
  /* SignalConversion: '<S10>/TmpSignal ConversionAtyInport1' incorporates:
   *  MATLAB Function: '<S10>/MATLAB Function'
   *  SignalConversion: '<S10>/TmpSignal ConversionAtXOutport1'
   */
  rty_y[0] = rtu_X[0];
  rty_y[1] = rtu_X[1];
}

/*
 * File trailer for generated code.
 *
 * [EOF]
 */

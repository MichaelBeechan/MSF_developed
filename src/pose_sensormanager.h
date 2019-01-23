/*
 * Copyright (C) 2012-2013 Simon Lynen, ASL, ETH Zurich, Switzerland
 * You can contact the author at <slynen at ethz dot ch>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef POSE_MEASUREMENTMANAGER_H
#define POSE_MEASUREMENTMANAGER_H

#include <ros/ros.h>

#include <msf_core/msf_core.h>
#include <msf_core/msf_sensormanagerROS.h>
#include <msf_core/msf_IMUHandler_ROS.h>
#include "msf_statedef.hpp"
//#include "custom_sensorhandler.h"
//#include "custom_measurement.h"
#include <msf_updates/pose_sensor_handler/pose_sensorhandler.h>
#include <msf_updates/pose_sensor_handler/pose_measurement.h>
#include <ai_robot_lcsfl/SinglePoseSensorConfig.h>

#include "sensor_fusion_comm/InitScale.h"
#include "sensor_fusion_comm/InitHeight.h"

namespace msf_pose_sensor {

typedef ai_robot_lcsfl::SinglePoseSensorConfig Config_T;
typedef dynamic_reconfigure::Server<Config_T> ReconfigureServer;
typedef shared_ptr<ReconfigureServer> ReconfigureServerPtr;

class PoseSensorManager : public msf_core::MSF_SensorManagerROS<
    msf_updates::EKFState> {
  typedef PoseSensorHandler<msf_updates::pose_measurement::PoseMeasurement<>,
      PoseSensorManager> PoseSensorHandler_T;
  typedef PoseSensorHandler<msf_updates::pose_measurement::PoseMeasurement<
    msf_updates::EKFState::StateDefinition_T::L2,
    msf_updates::EKFState::StateDefinition_T::q2_wv,
    msf_updates::EKFState::StateDefinition_T::p2_wv,
    msf_updates::EKFState::StateDefinition_T::q2_ic,
    msf_updates::EKFState::StateDefinition_T::p2_ic>,
      PoseSensorManager> PoseSensorHandler2_T;

  friend class PoseSensorHandler<msf_updates::pose_measurement::PoseMeasurement<>,
      PoseSensorManager> ;
  friend class PoseSensorHandler<msf_updates::pose_measurement::PoseMeasurement<
    msf_updates::EKFState::StateDefinition_T::L2,
    msf_updates::EKFState::StateDefinition_T::q2_wv,
    msf_updates::EKFState::StateDefinition_T::p2_wv,
    msf_updates::EKFState::StateDefinition_T::q2_ic,
    msf_updates::EKFState::StateDefinition_T::p2_ic>,
      PoseSensorManager> ;
 public:
  typedef msf_updates::EKFState EKFState_T;
  typedef EKFState_T::StateSequence_T StateSequence_T;
  typedef EKFState_T::StateDefinition_T StateDefinition_T;

  PoseSensorManager(ros::NodeHandle pnh = ros::NodeHandle("~/lcsfl_pose_sensor")) {
    bool distortmeas = false;  ///< Distort the pose measurements.

    imu_handler_.reset(
        new msf_core::IMUHandler_ROS<msf_updates::EKFState>(*this, "msf_core", "imu_handler"));
    pose_handler_.reset(
        new PoseSensorHandler_T(*this, "zed", "pose_sensor", distortmeas));
    pose_handler_2.reset(
        new PoseSensorHandler2_T(*this, "carto", "pose_sensor", distortmeas));

    AddHandler(pose_handler_);
    AddHandler(pose_handler_2);

    reconf_server_.reset(new ReconfigureServer(pnh));
    ReconfigureServer::CallbackType f = boost::bind(&PoseSensorManager::Config,
                                                    this, _1, _2);
    reconf_server_->setCallback(f);

    init_scale_srv_ = pnh.advertiseService("initialize_msf_scale",
                                           &PoseSensorManager::InitScale, this);
    init_height_srv_ = pnh.advertiseService("initialize_msf_height",
                                            &PoseSensorManager::InitHeight,
                                            this);
  }
  virtual ~PoseSensorManager() { }

  virtual const Config_T& Getcfg() {
    return config_;
  }

 private:
  shared_ptr<msf_core::IMUHandler_ROS<msf_updates::EKFState> > imu_handler_;
  shared_ptr<PoseSensorHandler_T> pose_handler_;
  shared_ptr<PoseSensorHandler2_T> pose_handler_2;

  Config_T config_;
  ReconfigureServerPtr reconf_server_;
  ros::ServiceServer init_scale_srv_;
  ros::ServiceServer init_height_srv_;

  /// Minimum initialization height. If a abs(height) is smaller than this value, 
  /// no initialization is performed.
  static constexpr double MIN_INITIALIZATION_HEIGHT = 0.01;

  /**
   * \brief Dynamic reconfigure callback.
   */
  virtual void Config(Config_T &config, uint32_t level) {
    config_ = config;
    pose_handler_->SetNoises(config.pose_noise_meas_p,
                             config.pose_noise_meas_q);
    pose_handler_->SetDelay(config.pose_delay);
    pose_handler_2->SetNoises(config.pose_noise_meas_p_2,
                             config.pose_noise_meas_q_2);
    pose_handler_2->SetDelay(config.pose_delay_2);
    if ((level & ai_robot_lcsfl::SinglePoseSensor_INIT_FILTER)
        && config.core_init_filter == true) {
      Init(config.pose_initial_scale);
      config.core_init_filter = false;
    }
    // Init call with "set height" checkbox.
    if ((level & ai_robot_lcsfl::SinglePoseSensor_SET_HEIGHT)
        && config.core_set_height == true) {
      Eigen::Matrix<double, 3, 1> p = pose_handler_->GetPositionMeasurement();
      if (p.norm() == 0) {
        MSF_WARN_STREAM(
            "No measurements received yet to initialize position. Height init "
            "not allowed.");
        return;
      }
      double scale = p[2] / config.core_height;
      Init(scale);
      config.core_set_height = false;
    }
  }

  bool InitScale(sensor_fusion_comm::InitScale::Request &req,
                 sensor_fusion_comm::InitScale::Response &res) {
    ROS_INFO("Initialize filter with scale %f", req.scale);
    Init(req.scale);
    res.result = "Initialized scale";
    return true;
  }

  bool InitHeight(sensor_fusion_comm::InitHeight::Request &req,
                  sensor_fusion_comm::InitHeight::Response &res) {
    ROS_INFO("Initialize filter with height %f", req.height);
    Eigen::Matrix<double, 3, 1> p = pose_handler_->GetPositionMeasurement();
    if (p.norm() == 0) {
      MSF_WARN_STREAM(
          "No measurements received yet to initialize position. Height init "
          "not allowed.");
      return false;
    }
    std::stringstream ss;
    if (std::abs(req.height) > MIN_INITIALIZATION_HEIGHT) {
      double scale = p[2] / req.height;
      Init(scale);
      ss << scale;
      res.result = "Initialized by known height. Initial scale = " + ss.str();
    } else {
      ss << "Height to small for initialization, the minimum is "
          << MIN_INITIALIZATION_HEIGHT << "and " << req.height << "was set.";
      MSF_WARN_STREAM(ss.str());
      res.result = ss.str();
      return false;
    }
    return true;
  }

  void Init(double scale) const {
    Eigen::Matrix<double, 3, 1> p, v, b_w, b_a, g, w_m, a_m, p_ic, p2_ic, p_vc, p_wv, p2_wv;
    Eigen::Quaternion<double> q, q_wv, q_ic, q2_wv, q2_ic, q_cv;
    msf_core::MSF_Core<EKFState_T>::ErrorStateCov P;

    // init values
    g << 0, 0, 9.81;	        /// Gravity.
    b_w << 0, 0, 0;		/// Bias gyroscopes.
    b_a << 0, 0, 0;		/// Bias accelerometer.

    v << 0, 0, 0;			/// Robot velocity (IMU centered).
    w_m << 0, 0, 0;		/// Initial angular velocity.

    q_wv.setIdentity();  // Vision-world rotation drift.
    p_wv.setZero();  // Vision-world position drift.
    q2_wv.setIdentity();  // Vision-world rotation drift.
    p2_wv.setZero();  // Vision-world position drift.

    P.setZero();  // Error state covariance; if zero, a default initialization in msf_core is used

    p_vc = pose_handler_->GetPositionMeasurement();
    q_cv = pose_handler_->GetAttitudeMeasurement();

    MSF_INFO_STREAM(
        "initial measurement pos:["<<p_vc.transpose()<<"] orientation: "<<STREAMQUAT(q_cv));

    // Check if we have already input from the measurement sensor.
    if (p_vc.norm() == 0)
      MSF_WARN_STREAM(
          "No measurements received yet to initialize position - using [0 0 0]");
    if (q_cv.w() == 1)
      MSF_WARN_STREAM(
          "No measurements received yet to initialize attitude - using [1 0 0 0]");

    ros::NodeHandle pnh("~");
    pnh.param("pose_sensor/init/p_ic/x", p_ic[0], 0.0);
    pnh.param("pose_sensor/init/p_ic/y", p_ic[1], 0.0);
    pnh.param("pose_sensor/init/p_ic/z", p_ic[2], 0.0);

    pnh.param("pose_sensor/init/q_ic/w", q_ic.w(), 1.0);
    pnh.param("pose_sensor/init/q_ic/x", q_ic.x(), 0.0);
    pnh.param("pose_sensor/init/q_ic/y", q_ic.y(), 0.0);
    pnh.param("pose_sensor/init/q_ic/z", q_ic.z(), 0.0);
    q_ic.normalize();

    pnh.param("pose_sensor/init/p2_ic/x", p2_ic[0], 0.0);
    pnh.param("pose_sensor/init/p2_ic/y", p2_ic[1], 0.0);
    pnh.param("pose_sensor/init/p2_ic/z", p2_ic[2], 0.0);

    pnh.param("pose_sensor/init/q2_ic/w", q2_ic.w(), 1.0);
    pnh.param("pose_sensor/init/q2_ic/x", q2_ic.x(), 0.0);
    pnh.param("pose_sensor/init/q2_ic/y", q2_ic.y(), 0.0);
    pnh.param("pose_sensor/init/q2_ic/z", q2_ic.z(), 0.0);
    q2_ic.normalize();

    // Calculate initial attitude and position based on sensor measurements.
    if (!pose_handler_->ReceivedFirstMeasurement()) {  // If there is no pose measurement, only apply q_wv.
      q = q_wv;
    } else {  // If there is a pose measurement, apply q_ic and q_wv to get initial attitude.
      q = (q_ic * q_cv.conjugate() * q_wv).conjugate();
    }

    q.normalize();
    p = p_wv + q_wv.conjugate().toRotationMatrix() * p_vc / scale
        - q.toRotationMatrix() * p_ic;

    a_m = q.inverse() * g;			/// Initial acceleration.

    // Prepare init "measurement"
    // True means that this message contains initial sensor readings.
    shared_ptr < msf_core::MSF_InitMeasurement<EKFState_T>>
            meas(new msf_core::MSF_InitMeasurement<EKFState_T>(true));

    meas->SetStateInitValue < StateDefinition_T::p > (p);
    meas->SetStateInitValue < StateDefinition_T::v > (v);
    meas->SetStateInitValue < StateDefinition_T::q > (q);
    meas->SetStateInitValue < StateDefinition_T::b_w > (b_w);
    meas->SetStateInitValue < StateDefinition_T::b_a > (b_a);
    meas->SetStateInitValue < StateDefinition_T::L> (Eigen::Matrix<double, 1, 1>::Constant(scale));
    meas->SetStateInitValue < StateDefinition_T::q_wv > (q_wv);
    meas->SetStateInitValue < StateDefinition_T::p_wv > (p_wv);
    meas->SetStateInitValue < StateDefinition_T::q_ic > (q_ic);
    meas->SetStateInitValue < StateDefinition_T::p_ic > (p_ic);
    meas->SetStateInitValue < StateDefinition_T::L2> (Eigen::Matrix<double, 1, 1>::Constant(scale));
    meas->SetStateInitValue < StateDefinition_T::q2_wv > (q2_wv);
    meas->SetStateInitValue < StateDefinition_T::p2_wv > (p2_wv);
    meas->SetStateInitValue < StateDefinition_T::q2_ic > (q2_ic);
    meas->SetStateInitValue < StateDefinition_T::p2_ic > (p2_ic);

    SetStateCovariance(meas->GetStateCovariance());  // Call my set P function.
    meas->Getw_m() = w_m;
    meas->Geta_m() = a_m;
    meas->time = ros::Time::now().toSec();

    // Call initialization in core.
    msf_core_->Init(meas);

  }

  // Prior to this call, all states are initialized to zero/identity.
  virtual void ResetState(EKFState_T& state) const {
    //set scale to 1
    Eigen::Matrix<double, 1, 1> scale;
    scale << 1.0;
    state.Set < StateDefinition_T::L > (scale);
  }
  virtual void InitState(EKFState_T& state) const {
    UNUSED(state);
  }

  virtual void CalculateQAuxiliaryStates(EKFState_T& state, double dt) const {
    const msf_core::Vector3 nqwvv = msf_core::Vector3::Constant(
        config_.pose_noise_q_wv);
    const msf_core::Vector3 npwvv = msf_core::Vector3::Constant(
        config_.pose_noise_p_wv);
    const msf_core::Vector3 nqicv = msf_core::Vector3::Constant(
        config_.pose_noise_q_ic);
    const msf_core::Vector3 npicv = msf_core::Vector3::Constant(
        config_.pose_noise_p_ic);
    const msf_core::Vector1 n_L = msf_core::Vector1::Constant(
        config_.pose_noise_scale);
    const msf_core::Vector3 nqwvv2 = msf_core::Vector3::Constant(
        config_.pose_noise_q_wv);
    const msf_core::Vector3 npwvv2 = msf_core::Vector3::Constant(
        config_.pose_noise_p_wv);
    const msf_core::Vector3 nqicv2 = msf_core::Vector3::Constant(
        config_.pose_noise_q_ic);
    const msf_core::Vector3 npicv2 = msf_core::Vector3::Constant(
        config_.pose_noise_p_ic);
    const msf_core::Vector1 n_L2 = msf_core::Vector1::Constant(
        config_.pose_noise_scale);

    // Compute the blockwise Q values and store them with the states,
    // these then get copied by the core to the correct places in Qd.
    state.GetQBlock<StateDefinition_T::L>() = (dt * n_L.cwiseProduct(n_L))
        .asDiagonal();
    state.GetQBlock<StateDefinition_T::q_wv>() =
        (dt * nqwvv.cwiseProduct(nqwvv)).asDiagonal();
    state.GetQBlock<StateDefinition_T::p_wv>() =
        (dt * npwvv.cwiseProduct(npwvv)).asDiagonal();
    state.GetQBlock<StateDefinition_T::q_ic>() =
        (dt * nqicv.cwiseProduct(nqicv)).asDiagonal();
    state.GetQBlock<StateDefinition_T::p_ic>() =
        (dt * npicv.cwiseProduct(npicv)).asDiagonal();

    state.GetQBlock<StateDefinition_T::L2>() = (dt * n_L2.cwiseProduct(n_L2))
        .asDiagonal();
    state.GetQBlock<StateDefinition_T::q2_wv>() =
        (dt * nqwvv2.cwiseProduct(nqwvv2)).asDiagonal();
    state.GetQBlock<StateDefinition_T::p2_wv>() =
        (dt * npwvv2.cwiseProduct(npwvv2)).asDiagonal();
    state.GetQBlock<StateDefinition_T::q2_ic>() =
        (dt * nqicv2.cwiseProduct(nqicv2)).asDiagonal();
    state.GetQBlock<StateDefinition_T::p2_ic>() =
        (dt * npicv2.cwiseProduct(npicv2)).asDiagonal();

  }

  virtual void SetStateCovariance(
      Eigen::Matrix<double, EKFState_T::nErrorStatesAtCompileTime,
          EKFState_T::nErrorStatesAtCompileTime>& P) const {
    UNUSED(P);
    // Nothing, we only use the simulated cov for the core plus diagonal for the
    // rest.
  }

  virtual void AugmentCorrectionVector(
      Eigen::Matrix<double, EKFState_T::nErrorStatesAtCompileTime, 1>& correction) const {
    UNUSED(correction);
  }

  virtual void SanityCheckCorrection(
      EKFState_T& delaystate,
      const EKFState_T& buffstate,
      Eigen::Matrix<double, EKFState_T::nErrorStatesAtCompileTime, 1>& correction) const {
    UNUSED(buffstate);
    UNUSED(correction);

    const EKFState_T& state = delaystate;
    if (state.Get<StateDefinition_T::L>()(0) < 0) {
      MSF_WARN_STREAM_THROTTLE(
          1,
          "Negative scale detected: " << state.Get<StateDefinition_T::L>()(0) << ". Correcting to 0.1");
      Eigen::Matrix<double, 1, 1> L_;
      L_ << 0.1;
      delaystate.Set < StateDefinition_T::L > (L_);
    }
    if (state.Get<StateDefinition_T::L2>()(0) < 0) {
      MSF_WARN_STREAM_THROTTLE(
          1,
          "Negative scale detected: " << state.Get<StateDefinition_T::L>()(0) << ". Correcting to 0.1");
      Eigen::Matrix<double, 1, 1> L2_;
      L2_ << 0.1;
      delaystate.Set < StateDefinition_T::L2 > (L2_);
    }
  }
};

}
#endif // POSE_MEASUREMENTMANAGER_H
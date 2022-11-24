// -*-c++-*--------------------------------------------------------------------
// Copyright 2020 Bernd Pfrommer <bernd.pfrommer@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <flir_spinnaker_ros2/camera_driver.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <image_transport/image_transport.hpp>
#include <iomanip>
#include <iostream>
#include <rclcpp_components/register_node_macro.hpp>
#include <sensor_msgs/fill_image.hpp>
#include <sensor_msgs/image_encodings.hpp>
#include <type_traits>

#include "logging.h"

namespace flir_spinnaker_ros2
{
namespace chrono = std::chrono;
//
// this complicated code is to detect an interface change
// between foxy and galactic
// See  https://stackoverflow.com/questions/1005476/
//  how-to-detect-whether-there-is-a-specific-member-variable-in-class

template <typename T, typename = bool>
struct DescSetter
{
  // don't set by default (foxy)
  static void set_dynamic_typing(T *) {}
};

template <typename T>
struct DescSetter<T, decltype((void)T::dynamic_typing, true)>
{
  // set if dynamic_typing is present
  static void set_dynamic_typing(T * desc) { desc->dynamic_typing = true; }
};

static rcl_interfaces::msg::ParameterDescriptor make_desc(
  const std::string name, int type)
{
  rcl_interfaces::msg::ParameterDescriptor desc;
  desc.name = name;
  desc.type = type;
  desc.description = name;
  DescSetter<rcl_interfaces::msg::ParameterDescriptor>::set_dynamic_typing(
    &desc);
  return (desc);
}

static std::pair<bool, double> get_double_int_param(const rclcpp::Parameter & p)
{
  std::pair<bool, double> bd(false, 0);
  if (p.get_type() == rclcpp::PARAMETER_DOUBLE) {
    bd.second = p.as_double();
    bd.first = true;
  }
  if (p.get_type() == rclcpp::PARAMETER_INTEGER) {
    bd.second = static_cast<double>(p.as_int());
    bd.first = true;
  }
  return (bd);
}

static std::pair<bool, bool> get_bool_int_param(const rclcpp::Parameter & p)
{
  std::pair<bool, bool> bb(false, false);
  if (p.get_type() == rclcpp::PARAMETER_BOOL) {
    bb.second = p.as_bool();
    bb.first = true;
  }
  if (p.get_type() == rclcpp::PARAMETER_INTEGER) {
    bb.second = static_cast<bool>(p.as_int());
    bb.first = true;
  }
  return (bb);
}

CameraDriver::NodeInfo::NodeInfo(
  const std::string & n, const std::string & nodeType)
: name(n)
{
  if (nodeType == "float") {
    type = FLOAT;
    descriptor = make_desc(n, rclcpp::ParameterType::PARAMETER_DOUBLE);
  } else if (nodeType == "int") {
    type = INT;
    descriptor = make_desc(n, rclcpp::ParameterType::PARAMETER_INTEGER);
  } else if (nodeType == "bool") {
    type = BOOL;
    descriptor = make_desc(n, rclcpp::ParameterType::PARAMETER_BOOL);
  } else if (nodeType == "enum") {
    type = ENUM;
    descriptor = make_desc(n, rclcpp::ParameterType::PARAMETER_STRING);
  }
}

CameraDriver::CameraDriver(const rclcpp::NodeOptions & options)
: Node("cam_sync", options)
{
  lastStatusTime_ = now();
  statusTimer_ = rclcpp::create_timer(
    this, get_clock(), rclcpp::Duration(5, 0),
    std::bind(&CameraDriver::printStatus, this));
  bool status = start();
  if (!status) {
    LOG_ERROR("startup failed!");
    throw std::runtime_error("startup of CameraDriver node failed!");
  }
}

CameraDriver::~CameraDriver()
{
  stop();
  driver_.reset();  // invoke destructor
}

bool CameraDriver::stop()
{
  stopCamera();
  if (driver_) {
    driver_->deInitCamera();
  }
  if (!statusTimer_->is_canceled()) {
    statusTimer_->cancel();
  }
  keepRunning_ = false;
  if (thread_) {
    thread_->join();
    thread_ = 0;
  }
  return (true);
}

bool CameraDriver::stopCamera()
{
  if (cameraRunning_ && driver_) {
    cameraRunning_ = false;
    return driver_->stopCamera();
  }
  return false;
}

void CameraDriver::printStatus()
{
  if (driver_) {
    const double dropRate = (publishedCount_ > 0)
                              ? (static_cast<double>(droppedCount_) /
                                 static_cast<double>(publishedCount_))
                              : 0;
    const rclcpp::Time t = now();
    const rclcpp::Duration dt = t - lastStatusTime_;
    double dtns = std::max(dt.nanoseconds(), (int64_t)1);
    double outRate = publishedCount_ * 1e9 / dtns;
    LOG_INFO(
      "frame rate in: " << driver_->getReceiveFrameRate() << " Hz, out:"
                        << outRate << " Hz, drop: " << dropRate * 100 << "%");
    lastStatusTime_ = t;
    droppedCount_ = 0;
    publishedCount_ = 0;
  } else {
    LOG_WARN("camera " << serial_ << " is not online!");
  }
}

void CameraDriver::readParameters()
{
  serial_ = this->declare_parameter<std::string>(
    "serial_number", "missing_serial_number");
  try {
    debug_ = this->declare_parameter(
      "debug", false,
      make_desc("debug", rclcpp::ParameterType::PARAMETER_BOOL));
  } catch (const rclcpp::exceptions::InvalidParameterTypeException & e) {
    LOG_WARN("bad debug param type: " << e.what());
    debug_ = false;
  }
  LOG_INFO("debug: " << debug_);
  cameraInfoURL_ = this->declare_parameter<std::string>("camerainfo_url", "");
  frameId_ = this->declare_parameter<std::string>("frame_id", get_name());
  dumpNodeMap_ = this->declare_parameter<bool>("dump_node_map", false);
  qosDepth_ = this->declare_parameter<int>("image_queue_size", 4);
  computeBrightness_ =
    this->declare_parameter<bool>("compute_brightness", false);
  acquisitionTimeout_ =
    this->declare_parameter<double>("acquisition_timeout", 3.0);
  parameterFile_ =
    this->declare_parameter<std::string>("parameter_file", "parameters.cfg");
  LOG_INFO(" serial: " << serial_);
  callbackHandle_ = this->add_on_set_parameters_callback(
    std::bind(&CameraDriver::parameterChanged, this, std::placeholders::_1));
}

bool CameraDriver::readParameterFile()
{
  std::ifstream f(parameterFile_);
  if (!f.is_open()) {
    LOG_ERROR("cannot read parameter definition file: " << parameterFile_);
    return (false);
  }
  std::string l;
  while (getline(f, l)) {
    std::istringstream iss(l);
    std::string s;
    std::vector<std::string> tokens;
    while (iss >> std::quoted(s)) {
      tokens.push_back(s);
    }
    if (!tokens.empty()) {
      if (!tokens[0].empty() && tokens[0][0] == '#') {
        continue;
      }
      if (tokens.size() != 3) {
        LOG_WARN("skipping bad camera param line: " << l);
        continue;
      }
      parameterMap_.insert({tokens[0], NodeInfo(tokens[2], tokens[1])});
      parameterList_.push_back(tokens[0]);
    }
  }
  f.close();
  return (true);
}

void CameraDriver::createCameraParameters()
{
  for (const auto & name : parameterList_) {
    const auto it = parameterMap_.find(name);
    if (it != parameterMap_.end()) {
      const auto & ni = it->second;  // should always succeed
      try {
        this->declare_parameter(
          name, rclcpp::ParameterValue(), ni.descriptor, false);
      } catch (rclcpp::exceptions::InvalidParameterTypeException & e) {
        LOG_WARN(
          "overwriting bad param with default: " + std::string(e.what()));
        this->declare_parameter(
          name, rclcpp::ParameterValue(), ni.descriptor, true);
      }
    }
  }
}

bool CameraDriver::setEnum(const std::string & nodeName, const std::string & v)
{
  LOG_INFO("setting " << nodeName << " to: " << v);
  std::string retV;  // what actually was set
  std::string msg = driver_->setEnum(nodeName, v, &retV);
  bool status(true);
  if (msg != "OK") {
    LOG_WARN("setting " << nodeName << " failed: " << msg);
    status = false;
  }
  if (v != retV) {
    LOG_WARN(nodeName << " set to: " << retV << " instead of: " << v);
    status = false;
  }
  return (status);
}

bool CameraDriver::setDouble(const std::string & nodeName, double v)
{
  LOG_INFO("setting " << nodeName << " to: " << v);
  double retV;  // what actually was set
  std::string msg = driver_->setDouble(nodeName, v, &retV);
  bool status(true);
  if (msg != "OK") {
    LOG_WARN("setting " << nodeName << " failed: " << msg);
    status = false;
  }
  if (std::abs(v - retV) > 0.025 * std::abs(v + retV)) {
    LOG_WARN(nodeName << " set to: " << retV << " instead of: " << v);
    status = false;
  }
  return (status);
}

bool CameraDriver::setInt(const std::string & nodeName, int v)
{
  LOG_INFO("setting " << nodeName << " to: " << v);
  int retV;  // what actually was set
  std::string msg = driver_->setInt(nodeName, v, &retV);
  bool status(true);
  if (msg != "OK") {
    LOG_WARN("setting " << nodeName << " failed: " << msg);
    status = false;
  }
  if (v != retV) {
    LOG_WARN(nodeName << " set to: " << retV << " instead of: " << v);
    status = false;
  }
  return (status);
}

bool CameraDriver::setBool(const std::string & nodeName, bool v)
{
  LOG_INFO("setting " << nodeName << " to: " << v);
  bool retV;  // what actually was set
  std::string msg = driver_->setBool(nodeName, v, &retV);
  bool status(true);
  if (msg != "OK") {
    LOG_WARN("setting " << nodeName << " failed: " << msg);
    status = false;
  }
  if (v != retV) {
    LOG_WARN(nodeName << " set to: " << retV << " instead of: " << v);
    status = false;
  }
  return (status);
}

void CameraDriver::setParameter(
  const NodeInfo & ni, const rclcpp::Parameter & p)
{
  switch (ni.type) {
    case NodeInfo::ENUM: {
      std::string s = p.value_to_string();
      // remove quotes
      s.erase(remove(s.begin(), s.end(), '\"'), s.end());
      setEnum(ni.name, s);
      break;
    }
    case NodeInfo::FLOAT: {
      auto bd = get_double_int_param(p);
      if (bd.first) {
        setDouble(ni.name, bd.second);
      } else {
        LOG_WARN("bad non-float " << p.get_name() << " type: " << p.get_type());
      }
      break;
    }
    case NodeInfo::INT: {
      auto bd = get_double_int_param(p);
      if (bd.first) {
        setInt(ni.name, bd.second);
      } else {
        LOG_WARN("bad non-int " << p.get_name() << " type: " << p.get_type());
      }
      break;
    }
    case NodeInfo::BOOL: {
      auto bb = get_bool_int_param(p);
      if (bb.first) {
        setBool(ni.name, bb.second);
      } else {
        LOG_WARN("bad non-bool " << p.get_name() << " type: " << p.get_type());
      }
      break;
    }
    default:
      LOG_WARN("invalid node type in map: " << ni.type);
  }
}

rcl_interfaces::msg::SetParametersResult CameraDriver::parameterChanged(
  const std::vector<rclcpp::Parameter> & params)
{
  for (const auto & p : params) {
    const auto it = parameterMap_.find(p.get_name());
    if (it == parameterMap_.end()) {
      continue;  // ignore unknown param
    }
    if (!driver_) {
      LOG_WARN("got parameter update while driver is not ready!");
      continue;
    }
    const NodeInfo & ni = it->second;
    if (p.get_type() == rclcpp::PARAMETER_NOT_SET) {
      continue;
    }
    try {
      setParameter(ni, p);
    } catch (const flir_spinnaker_common::Driver::DriverException & e) {
      LOG_WARN("param " << p.get_name() << " " << e.what());
    }
  }
  rcl_interfaces::msg::SetParametersResult res;
  res.successful = true;
  res.reason = "all good!";
  return (res);
}

void CameraDriver::controlCallback(
  const camera_control_msgs_ros2::msg::CameraControl::UniquePtr msg)
{
  /*
  LOG_INFO(
    "control msg: time: " << currentExposureTime_ << " -> "
                          << msg->exposure_time << " gain: " << currentGain_
                          << " -> " << msg->gain); */
  const uint32_t et = msg->exposure_time;
  const float gain = msg->gain;
  bool logTime(false);
  bool logGain(false);
  try {
    if (et > 0 && et != currentExposureTime_) {
      const auto it = parameterMap_.find("exposure_time");
      if (it != parameterMap_.end()) {
        const auto & ni = it->second;
        setDouble(ni.name, et);
        currentExposureTime_ = et;
        logTime = true;
      } else {
        LOG_WARN("no node name defined for exposure_time, check .cfg file!");
      }
    }
    if (gain > std::numeric_limits<float>::lowest() && gain != currentGain_) {
      const auto it = parameterMap_.find("gain");
      if (it != parameterMap_.end()) {
        const auto & ni = it->second;
        setDouble(ni.name, gain);
        currentGain_ = gain;
        logGain = true;
      } else {
        LOG_WARN("no node name defined for exposure_time, check .cfg file!");
      }
    }
  } catch (const flir_spinnaker_common::Driver::DriverException & e) {
    LOG_WARN("failed to control: " << e.what());
  }

  if (logTime) {
    LOG_INFO("changed exposure time to " << et << "us");
  }
  if (logGain) {
    LOG_INFO("changed gain to " << gain << "db");
  }
}

void CameraDriver::publishImage(const ImageConstPtr & im)
{
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (imageQueue_.size() < 2) {
      imageQueue_.push_back(im);
      cv_.notify_all();
    } else {
      droppedCount_++;
    }
  }
}

void CameraDriver::run()
{
  while (keepRunning_ && rclcpp::ok()) {
    {
      ImageConstPtr img;
      {  // ------- locked section ---
        std::unique_lock<std::mutex> lock(mutex_);
        // one second timeout
        const std::chrono::microseconds timeout((int64_t)(1000000LL));
        while (imageQueue_.empty() && keepRunning_ && rclcpp::ok()) {
          cv_.wait_for(lock, timeout);
        }
        if (!imageQueue_.empty()) {
          img = imageQueue_.back();
          imageQueue_.pop_back();
        }
      }  // -------- end of locked section
      if (img && keepRunning_ && rclcpp::ok()) {
        doPublish(img);
      }
    }
  }
}

static std::string flir_to_ros_encoding(
  const flir_spinnaker_common::pixel_format::PixelFormat & pf)
{
  switch (pf) {
    case flir_spinnaker_common::pixel_format::BayerRG8:
      return (sensor_msgs::image_encodings::BAYER_RGGB8);
      break;
    case flir_spinnaker_common::pixel_format::RGB8:
      return (sensor_msgs::image_encodings::RGB8);
      break;
    case flir_spinnaker_common::pixel_format::Mono8:
      return (sensor_msgs::image_encodings::MONO8);
      break;
    case flir_spinnaker_common::pixel_format::INVALID:
    default:
      return ("INVALID");
  }
}

void CameraDriver::doPublish(const ImageConstPtr & im)
{
  // todo: honor the encoding in the image
  const rclcpp::Time t(im->imageTime_);
  // const auto t = now();
  imageMsg_.header.stamp = t;
  cameraInfoMsg_.header.stamp = t;

  const std::string encoding = flir_to_ros_encoding(im->pixelFormat_);

  if (count_subscribers(pub_.getTopic()) > 0) {
    sensor_msgs::msg::CameraInfo::UniquePtr cinfo(
      new sensor_msgs::msg::CameraInfo(cameraInfoMsg_));
    // will make deep copy. Do we need to? Probably...
    sensor_msgs::msg::Image::UniquePtr img(
      new sensor_msgs::msg::Image(imageMsg_));
    bool ret = sensor_msgs::fillImage(
      *img, encoding, im->height_, im->width_, im->stride_, im->data_);
    if (!ret) {
      LOG_ERROR("fill image failed!");
    } else {
      // const auto t0 = this->now();
      pub_.publish(std::move(img), std::move(cinfo));
      // const auto t1 = this->now();
      // std::cout << "dt: " << (t1 - t0).nanoseconds() * 1e-9 << std::endl;
      publishedCount_++;
    }
  }
  if (metaPub_->get_subscription_count() != 0) {
    metaMsg_.header.stamp = t;
    metaMsg_.brightness = im->brightness_;
    metaMsg_.exposure_time = im->exposureTime_;
    metaMsg_.max_exposure_time = im->maxExposureTime_;
    metaMsg_.gain = im->gain_;
    metaMsg_.camera_time = im->imageTime_;
    metaPub_->publish(metaMsg_);
  }
}

void CameraDriver::printCameraInfo()
{
  if (cameraRunning_) {
    LOG_INFO("camera has pixel format: " << driver_->getPixelFormat());
  }
}

void CameraDriver::startCamera()
{
  if (!cameraRunning_) {
    flir_spinnaker_common::Driver::Callback cb =
      std::bind(&CameraDriver::publishImage, this, std::placeholders::_1);
    cameraRunning_ = driver_->startCamera(cb);
    if (!cameraRunning_) {
      LOG_ERROR("failed to start camera!");
    } else {
      printCameraInfo();
    }
  }
}

bool CameraDriver::start()
{
  readParameters();
  if (!readParameterFile()) {
    return (false);
  }
  infoManager_ = std::make_shared<camera_info_manager::CameraInfoManager>(
    this, get_name(), cameraInfoURL_);
  controlSub_ =
    this->create_subscription<camera_control_msgs_ros2::msg::CameraControl>(
      "~/control", 10,
      std::bind(&CameraDriver::controlCallback, this, std::placeholders::_1));
  metaPub_ =
    create_publisher<image_meta_msgs_ros2::msg::ImageMetaData>("~/meta", 1);

  cameraInfoMsg_ = infoManager_->getCameraInfo();
  imageMsg_.header.frame_id = frameId_;
  cameraInfoMsg_.header.frame_id = frameId_;
  metaMsg_.header.frame_id = frameId_;

  rmw_qos_profile_t qosProf = rmw_qos_profile_default;
  qosProf.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  qosProf.depth = qosDepth_;  // keep at most this number of images

  qosProf.reliability = RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT;
  // qosProf.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;

  // qosProf.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
  // qosProf.durability = RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT;
  qosProf.durability =
    RMW_QOS_POLICY_DURABILITY_VOLATILE;  // sender does not have to store
  qosProf.deadline.sec = 5;              // max expect time between msgs pub
  qosProf.deadline.nsec = 0;

  qosProf.lifespan.sec = 1;  // how long until msg are considered expired
  qosProf.lifespan.nsec = 0;

  qosProf.liveliness_lease_duration.sec = 10;  // time to declare client dead
  qosProf.liveliness_lease_duration.nsec = 0;

  pub_ = image_transport::create_camera_publisher(this, "~/image_raw", qosProf);
  driver_ = std::make_shared<flir_spinnaker_common::Driver>();
  driver_->setDebug(debug_);
  driver_->setComputeBrightness(computeBrightness_);
  driver_->setAcquisitionTimeout(acquisitionTimeout_);

  LOG_INFO("using spinnaker lib version: " + driver_->getLibraryVersion());
  bool foundCamera = false;
  for (int retry = 1; retry < 6; retry++) {
    driver_->refreshCameraList();
    const auto camList = driver_->getSerialNumbers();
    if (std::find(camList.begin(), camList.end(), serial_) == camList.end()) {
      LOG_WARN(
        "no camera found with serial: " << serial_ << " on try # " << retry);
      for (const auto & cam : camList) {
        LOG_WARN("found cameras: " << cam);
      }
      std::this_thread::sleep_for(chrono::seconds(1));
    } else {
      LOG_INFO("found camera with serial number: " << serial_);
      foundCamera = true;
      break;
    }
  }
  if (!foundCamera) {
    LOG_ERROR("giving up, camera " << serial_ << " not found!");
    return (false);
  }
  keepRunning_ = true;
  thread_ = std::make_shared<std::thread>(&CameraDriver::run, this);

  if (driver_->initCamera(serial_)) {
    if (dumpNodeMap_) {
      LOG_INFO("dumping node map!");
      std::string nm = driver_->getNodeMapAsString();
      std::cout << nm;
    }
    // Must first create the camera parameters before acquisition is started.
    // Some parameters (like blackfly s chunk control) cannot be set once
    // the camera is running.
    createCameraParameters();
    // TODO(bernd): once ROS2 supports subscriber status callbacks, this can go!
    startCamera();
  } else {
    LOG_ERROR("init camera failed for cam: " << serial_);
  }
  return (true);
}
}  // namespace flir_spinnaker_ros2

RCLCPP_COMPONENTS_REGISTER_NODE(flir_spinnaker_ros2::CameraDriver)

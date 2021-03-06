/******************************************************************************
 * Copyright (C) 2014 by Jerome Maye                                          *
 * jerome.maye@gmail.com                                                      *
 *                                                                            *
 * This program is free software; you can redistribute it and/or modify       *
 * it under the terms of the Lesser GNU General Public License as published by*
 * the Free Software Foundation; either version 3 of the License, or          *
 * (at your option) any later version.                                        *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * Lesser GNU General Public License for more details.                        *
 *                                                                            *
 * You should have received a copy of the Lesser GNU General Public License   *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.       *
 ******************************************************************************/

#include "VelodynePostNode.h"

#include <sstream>
#include <iomanip>
#include <cstring>
#include <cmath>
#include <fstream>

#include <boost/make_shared.hpp>

#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/point_cloud_conversion.h>

#include <libsnappy/snappy.h>

#include <libvelodyne/sensor/DataPacket.h>
#include <libvelodyne/sensor/Calibration.h>
#include <libvelodyne/sensor/Converter.h>
#include <libvelodyne/data-structures/VdynePointCloud.h>
#include <libvelodyne/exceptions/IOException.h>

namespace velodyne {

/******************************************************************************/
/* Constructors and Destructor                                                */
/******************************************************************************/

  VelodynePostNode::VelodynePostNode(const ros::NodeHandle& nh) :
      _nodeHandle(nh),
      _subscriptionIsActive(false) {
    getParameters();
    std::ifstream calibFile(_calibFileName);
    _calibration = std::make_shared<Calibration>();
    try {
      calibFile >> *_calibration;
    }
    catch (const IOException& e) {
      ROS_WARN_STREAM("IOException: " << e.what());
    }
    if (_transportType == "udp")
      _transportHints = ros::TransportHints().unreliable().reliable();
    else if (_transportType == "tcp")
      _transportHints = ros::TransportHints().reliable().unreliable();
    else
      ROS_ERROR_STREAM("Unknown transport type: " << _transportType);
    _pointCloudPublisher = _nodeHandle.advertise<sensor_msgs::PointCloud2>(
      _pointCloudTopicName, _queueDepth);
    _dataPackets.reserve(_numDataPackets);
  }

  VelodynePostNode::~VelodynePostNode() {
  }

/******************************************************************************/
/* Methods                                                                    */
/******************************************************************************/

  void VelodynePostNode::velodyneDataPacketCallback(const
      velodyne::DataPacketMsgConstPtr& msg) {
    _frameId = msg->header.frame_id;
    DataPacket dataPacket;
    for (size_t i = 0; i < DataPacket::mDataChunkNbr; ++i) {
      DataPacket::DataChunk dataChunk;
      dataChunk.mHeaderInfo = msg->dataChunks[i].headerInfo;
      dataChunk.mRotationalInfo = msg->dataChunks[i].rotationalInfo;
      for (size_t j = 0; j < DataPacket::DataChunk::mLasersPerPacket; ++j) {
        DataPacket::LaserData laserData;
        laserData.mDistance = msg->dataChunks[i].laserData[j].distance;
        laserData.mIntensity = msg->dataChunks[i].laserData[j].intensity;
        dataChunk.mLaserData[j] = laserData;
      }
      dataPacket.setDataChunk(dataChunk, i);
    }
    dataPacket.setTimestamp(msg->header.stamp.toNSec());
    dataPacket.setSpinCount(msg->spinCount);
    dataPacket.setReserved(msg->reserved);
    _dataPackets.push_back(dataPacket);
    if (_dataPackets.size() == static_cast<size_t>(_numDataPackets)) {
      publish();
      _dataPackets.clear();
    }
  }

  void VelodynePostNode::velodyneBinarySnappyCallback(const
      velodyne::BinarySnappyMsgConstPtr& msg) {
    std::string uncompressedData;
    snappy::Uncompress(
      reinterpret_cast<const char*>(msg->data.data()),
      msg->data.size(), &uncompressedData);
    _frameId = msg->header.frame_id;
    DataPacket dataPacket;
    std::istringstream binaryStream(uncompressedData);
    dataPacket.readBinary(binaryStream);
    dataPacket.setTimestamp(msg->header.stamp.toNSec());
    _dataPackets.push_back(dataPacket);
    if (_dataPackets.size() == static_cast<size_t>(_numDataPackets)) {
      publish();
      _dataPackets.clear();
    }
  }

  void VelodynePostNode::publish() {
    if (_pointCloudPublisher.getNumSubscribers() == 0)
      return;
    VdynePointCloud pointCloud;
    for (auto it = _dataPackets.cbegin(); it != _dataPackets.cend(); ++it)
      Converter::toPointCloud(*it, *_calibration, pointCloud, _minDistance,
        _maxDistance);
    auto rosPointCloud = boost::make_shared<sensor_msgs::PointCloud>();
    rosPointCloud->header.stamp =
      ros::Time().fromNSec(_dataPackets.front().getTimestamp()
      + std::round((_dataPackets.back().getTimestamp() -
      _dataPackets.front().getTimestamp()) * 0.5));
    rosPointCloud->header.frame_id = _frameId;
    const auto numPoints = pointCloud.getSize();
    rosPointCloud->points.reserve(numPoints);
    rosPointCloud->channels.resize(1);
    rosPointCloud->channels[0].name = "intensity";
    rosPointCloud->channels[0].values.reserve(numPoints);
    const auto& points = pointCloud.getPoints();
    for (const auto& point : points) {
      geometry_msgs::Point32 rosPoint;
      rosPoint.x = point.mX;
      rosPoint.y = point.mY;
      rosPoint.z = point.mZ;
      rosPointCloud->points.push_back(rosPoint);
      rosPointCloud->channels[0].values.push_back(point.mIntensity);
    }
    auto rosPointCloud2 = boost::make_shared<sensor_msgs::PointCloud2>();
    convertPointCloudToPointCloud2 (*rosPointCloud, *rosPointCloud2);
    _pointCloudPublisher.publish(rosPointCloud2);
  }

  void VelodynePostNode::spin() {
    ros::spin();
  }

  void VelodynePostNode::getParameters() {
    _nodeHandle.param<double>("sensor/min_distance", _minDistance, 0.9);
    _nodeHandle.param<double>("sensor/max_distance", _maxDistance, 120.0);
    _nodeHandle.param<std::string>("sensor/device_name", _deviceName,
      "Velodyne HDL-32E");
    if (_deviceName == "Velodyne HDL-64E S2")
      _nodeHandle.param<std::string>("sensor/calibration_file", _calibFileName,
        "conf/calib-HDL-64E.dat");
    else if (_deviceName == "Velodyne HDL-32E")
      _nodeHandle.param<std::string>("sensor/calibration_file", _calibFileName,
        "conf/calib-HDL-32E.dat");
    else
      ROS_ERROR_STREAM("Unknown device: " << _deviceName);
    _nodeHandle.param<std::string>("ros/velodyne_binary_snappy_topic_name",
      _velodyneBinarySnappyTopicName, "/velodyne/binary_snappy");
    _nodeHandle.param<std::string>("ros/velodyne_data_packet_topic_name",
      _velodyneDataPacketTopicName, "/velodyne/data_packet");
    _nodeHandle.param<std::string>("ros/point_cloud_topic_name",
      _pointCloudTopicName, "point_cloud");
    _nodeHandle.param<bool>("ros/use_binary_snappy", _useBinarySnappy, true);
    _nodeHandle.param<int>("ros/queue_depth", _queueDepth, 100);
    _nodeHandle.param<std::string>("ros/transport_type", _transportType, "udp");
    if (_deviceName == "Velodyne HDL-64E S2")
      _nodeHandle.param<int>("ros/num_data_packets", _numDataPackets, 348);
    else if (_deviceName == "Velodyne HDL-32E")
      _nodeHandle.param<int>("ros/num_data_packets", _numDataPackets, 174);
    double rate;
    _nodeHandle.param<double>("ros/subscription_updater_rate", rate, 1.0);
    _timer = _nodeHandle.createTimer(ros::Duration(1.0 / rate),
      &VelodynePostNode::updateSubscription, this);
  }

  void VelodynePostNode::updateSubscription(const ros::TimerEvent& /*event*/) {
    if (_subscriptionIsActive &&
        _pointCloudPublisher.getNumSubscribers() == 0)
      shutdownSubscribers();
    else if (!_subscriptionIsActive &&
        _pointCloudPublisher.getNumSubscribers() > 0)
      initSubscribers();
  }

  void VelodynePostNode::initSubscribers() {
    if (_useBinarySnappy)
      _velodyneBinarySnappySubscriber =
        _nodeHandle.subscribe(_velodyneBinarySnappyTopicName,
        _queueDepth, &VelodynePostNode::velodyneBinarySnappyCallback, this,
        _transportHints);
    else
      _velodyneDataPacketSubscriber =
        _nodeHandle.subscribe(_velodyneDataPacketTopicName,
        _queueDepth, &VelodynePostNode::velodyneDataPacketCallback, this,
        _transportHints);
    _subscriptionIsActive = true;
  }

  void VelodynePostNode::shutdownSubscribers() {
    if (_useBinarySnappy)
      _velodyneBinarySnappySubscriber.shutdown();
    else
      _velodyneDataPacketSubscriber.shutdown();
    _subscriptionIsActive = false;
  }

}

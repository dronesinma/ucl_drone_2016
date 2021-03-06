/*!
 *  This file is part of ucl_drone 2016.
 *  For more information, refer to the corresponding header file.
 *
 *  \author Arnaud Jacques & Alexandre Leclere
 *  \date 2016
 *
 */

#include "ucl_drone/pose_estimation.h"

#include <math.h> /* isfinite*/

// Constructor
PoseEstimator::PoseEstimator()
{
  // Parameters

  ros::param::get("~use_visual_pose",
                  this->use_visual_pose);  // if false, only onboard readings are used

  std::string drone_prefix;
  ros::param::get("~drone_prefix", drone_prefix);  // relative path to the ardrone_autonomy node

  // Subsribers

  navdata_channel =
      nh.resolveName(drone_prefix + "ardrone/navdata");  // raw information from Parrot SDK
  navdata_sub = nh.subscribe(navdata_channel, 10, &PoseEstimator::navdataCb, this);

  odometry_channel =
      nh.resolveName(drone_prefix + "ardrone/odometry");  // information from Parrot SDK converted
                                                          // to SI Units by ardrone_autonomy
                                                          // + buggy velocities integration
  odometry_sub = nh.subscribe(odometry_channel, 10, &PoseEstimator::odometryCb, this);

  pose_visual_channel = nh.resolveName("pose_visual");  // pose estimation from the mapping node
  pose_visual_sub = nh.subscribe(pose_visual_channel, 1, &PoseEstimator::poseVisualCb, this);

  reset_channel = nh.resolveName("reset_pose");  // signal to reset the pose to (0,0,h)
  reset_sub = nh.subscribe(reset_channel, 10, &PoseEstimator::resetCb, this);

  // Publishers

  if (this->use_visual_pose)
  {
    pose_channel = nh.resolveName("pose_estimation");
    end_reset_pose_channel = nh.resolveName("end_reset_pose");
  }
  else
  {
    pose_channel = nh.resolveName("blind_pose_estimation");
    end_reset_pose_channel = nh.resolveName("blind_end_reset_pose");
  }

  pose_pub = nh.advertise< ucl_drone::Pose3D >(pose_channel, 1);

  end_reset_pose_pub = nh.advertise< std_msgs::Empty >(end_reset_pose_channel, 1);

  odometry_publishing = false;
  visual_pose_available = false;
  pending_reset = false;
}

// Destructor
PoseEstimator::~PoseEstimator()
{
}

void PoseEstimator::resetCb(const std_msgs::Empty msg)
{
  pending_reset = true;  // togle reset state when reset signal is emmitted
}

// this function call the drone to re-caibrate sensors ! do not call this when the drone is flying !
// the drone needs to lie flat on ground
void PoseEstimator::doFlatTrim()
{
  ros::ServiceClient client = nh.serviceClient< std_srvs::Empty >("motherboard1/ardrone/flattrim");
  std_srvs::Empty srv;
  client.call(srv);
}

// method to reset the pose to origin (behalve the altitude, the altimeter is used)
void PoseEstimator::doReset()
{
  odometry_x = 0;
  odometry_y = 0;
  odometry_rotX = 0;
  odometry_rotY = 0;
  odometry_rotZ = 0;
  odom_time = lastOdometryReceived.header.stamp;

  lastposeVisualReceived = ucl_drone::Pose3D();
  visual_pose_available = false;

  rot_Z_offset = lastNavdataReceived.rotZ / 180.0 * PI;
}

void PoseEstimator::navdataCb(const ardrone_autonomy::Navdata::ConstPtr navdataPtr)
{
  if (pending_reset)
    return;
  lastNavdataReceived = *navdataPtr;
}

void PoseEstimator::poseVisualCb(const ucl_drone::Pose3D::ConstPtr poseVisualPtr)
{
  if (pending_reset)
    return;

  // throw away values older than 1 sec
  if (poseVisualPtr->header.stamp < ros::Time::now() - ros::Duration(1.0 / 2.0))
  {
    ROS_DEBUG("VISUAL POSE disregarded in pose_estimation");
    return;
  }

  lastposeVisualReceived = *poseVisualPtr;
  visual_pose_available = true;
}

void PoseEstimator::odometryCb(const nav_msgs::Odometry::ConstPtr odometryPtr)
{
  // to see what exactly is put in Odometry: see in ardrone_autonomy:
  // ardrone_driver.cpp line 698 to 713

  // avoid to publish NAN values
  if (std::isfinite(odometryPtr->twist.twist.linear.x) &&
      std::isfinite(odometryPtr->twist.twist.linear.y))
  {
    odometry_publishing = true;
    lastOdometryReceived = *odometryPtr;
    previous_rotX = lastRotX;
    previous_rotY = lastRotY;
    previous_rotZ = lastRotZ;
    lastRotX = lastNavdataReceived.rotX / 180.0 * PI;
    lastRotY = lastNavdataReceived.rotY / 180.0 * PI;
    lastRotZ = lastNavdataReceived.rotZ / 180.0 * PI;
  }
  else
  {
    ROS_DEBUG("Is ardrone_autonomy odometry a stupid Nan ?");
  }
}

void PoseEstimator::publish_end_reset_pose()
{
  std_msgs::Empty msg;
  end_reset_pose_pub.publish(msg);
  pending_reset = false;
}

void PoseEstimator::publish_pose()
{
  ucl_drone::Pose3D pose_msg;
  bool fusion_is_ok = false;
  if (this->use_visual_pose)
  {
    fusion_is_ok = queuePoseFusion(pose_msg);
  }
  else
  {
    fusion_is_ok = poseFusion(pose_msg);
  }

  // publish
  if (fusion_is_ok)  // && pose_pub.getNumSubscribers() > 0)
  {
    pose_pub.publish(pose_msg);
  }
}

bool PoseEstimator::poseCopy(ucl_drone::Pose3D& pose_msg)
{
  // Stupid recopying of ardrone_autonomy odometry based on the integration of
  // velocity estimation based on Parrot's optical flow (bottom camera).
  // Some small differences:
  //    - the header timestamp gives the current time
  //    - the angles are given in Euler form (not quaternions): can be deduced
  //      from quaternions (can introduce NaN's) or by directly recopying Navdata
  pose_msg.header.stamp = ros::Time().now();  // lastOdometryReceived.header.stamp;
  pose_msg.header.frame_id = lastOdometryReceived.header.frame_id;
  pose_msg.x = lastOdometryReceived.pose.pose.position.x;
  pose_msg.y = lastOdometryReceived.pose.pose.position.y;
  pose_msg.z = lastOdometryReceived.pose.pose.position.z;

  pose_msg.rotX = lastNavdataReceived.rotX / 180.0 * PI;
  pose_msg.rotY = lastNavdataReceived.rotY / 180.0 * PI;
  pose_msg.rotZ = lastNavdataReceived.rotZ / 180.0 * PI;
  pose_msg.xvel = lastOdometryReceived.twist.twist.linear.x;
  pose_msg.yvel = lastOdometryReceived.twist.twist.linear.y;
  pose_msg.zvel = lastOdometryReceived.twist.twist.linear.z;
  pose_msg.rotXvel = lastOdometryReceived.twist.twist.angular.x;
  pose_msg.rotYvel = lastOdometryReceived.twist.twist.angular.y;
  pose_msg.rotZvel = lastOdometryReceived.twist.twist.angular.z;
}

bool PoseEstimator::poseFusion(ucl_drone::Pose3D& pose_msg)
{
  // with our own odometry integrated from optical flow
  // reset taken into account
  // but visual pose not used
  ros::Time previous_odom_time = odom_time;
  odom_time = lastOdometryReceived.header.stamp;
  double delta_t = (odom_time - previous_odom_time).toSec();

  if (delta_t == 0)  // it seems no new odometry was published
  {
    ROS_DEBUG("Is ardrone_autonomy always responding ?");
    return false;
  }
  if (!std::isfinite(delta_t))
  {
    ROS_DEBUG("Something strange hapenned! delta_t is not finite!");
    return false;
  }

  pose_msg.rotX = lastRotX;  // no fusion
  pose_msg.rotY = lastRotY;  // no fusion

  odometry_rotZ += lastRotZ - previous_rotZ;

  if (odometry_rotZ > M_PI)
    odometry_rotZ -= 2 * M_PI;
  if (odometry_rotZ < -M_PI)
    odometry_rotZ += 2 * M_PI;

  pose_msg.rotZ = odometry_rotZ;

  // Formulas recopied from ardrone_autonomy, to compute pose from integration
  // of the velocities based on Parrot's optical flow (bottom camera).
  double xvel =
      (float)((cos(pose_msg.rotZ) * (lastOdometryReceived.twist.twist.linear.x) -   // - 0.0048) -
               sin(pose_msg.rotZ) * (lastOdometryReceived.twist.twist.linear.y)) *  // - 0.0158)) *
              delta_t);
  odometry_x += xvel;

  double yvel =
      (float)((sin(pose_msg.rotZ) * (lastOdometryReceived.twist.twist.linear.x) +   // - 0.0048) +
               cos(pose_msg.rotZ) * (lastOdometryReceived.twist.twist.linear.y)) *  // - 0.0158)) *
              delta_t);
  odometry_y += yvel;

  pose_msg.x = odometry_x;
  pose_msg.y = odometry_y;
  pose_msg.z = lastOdometryReceived.pose.pose.position.z;  // no fusion
  // but altitude is not received when drone is not flying !
  if (lastOdometryReceived.pose.pose.position.z == 0)
    pose_msg.z = lastposeVisualReceived.z;  // for experiments

  pose_msg.header.stamp = odom_time;

  pose_msg.xvel = lastOdometryReceived.twist.twist.linear.x;
  pose_msg.yvel = lastOdometryReceived.twist.twist.linear.y;
  pose_msg.zvel =
      lastOdometryReceived.twist.twist.linear.z;  // not received when drone is not flying !
  pose_msg.rotXvel =
      lastOdometryReceived.twist.twist.angular.x;  // not received when drone is not flying !
  pose_msg.rotYvel =
      lastOdometryReceived.twist.twist.angular.y;  // not received when drone is not flying !
  pose_msg.rotZvel =
      lastOdometryReceived.twist.twist.angular.z;  // not received when drone is not flying !

  return true;
}

bool PoseEstimator::queuePoseFusion(ucl_drone::Pose3D& pose_msg)
{
  // Simple fusion technique explained in our report
  // our own odometry integrated from optical flow
  ros::Time previous_odom_time = odom_time;
  odom_time = lastOdometryReceived.header.stamp;
  double delta_t = (odom_time - previous_odom_time).toSec();

  if (delta_t == 0)  // it seems no new odometry was published
  {
    ROS_DEBUG("Is ardrone_autonomy always responding ?");
    return false;
  }
  if (!std::isfinite(delta_t))
  {
    ROS_DEBUG("Something strange hapenned! delta_t is not finite!");
    return false;
  }

  if (visual_pose_available)
  {
    ROS_DEBUG("VISUAL POSE received in pose_estimation");
    odometry_x = 0;
    odometry_y = 0;
    odometry_rotZ = lastposeVisualReceived.rotZ;

    this->processQueue(this->queue_dx, this->odometry_x);
    this->processQueue(this->queue_dy, this->odometry_y);
    this->processQueue(this->queue_drotZ, this->odometry_rotZ);

    visual_pose_available = false;
  }

  pose_msg.rotX = lastRotX;
  pose_msg.rotY = lastRotY;

  ////////////////////////////////////////////////////////////////////
  odometry_rotZ += lastRotZ - previous_rotZ;

  ////////////////////////////////////////////////////////////////////
  this->pushQueue(this->queue_drotZ, lastRotZ - previous_rotZ);
  pose_msg.rotZ = odometry_rotZ;

  // ////////////////////////////////////////////////////////////////////
  // pose_msg.rotZ = lastRotZ - rot_Z_offset;  // rot_Z_offset determined by

  // initial calibration
  // (see PoseEstimator::reset)

  if (pose_msg.rotZ > M_PI)
    pose_msg.rotZ -= 2 * M_PI;
  if (pose_msg.rotZ < -M_PI)
    pose_msg.rotZ += 2 * M_PI;

  // Formulas recopied from ardrone_autonomy, to compute pose from integration
  // of the velocities
  double dx =
      (float)((cos(pose_msg.rotZ) * (lastOdometryReceived.twist.twist.linear.x) -   // - 0.0048) -
               sin(pose_msg.rotZ) * (lastOdometryReceived.twist.twist.linear.y)) *  // - 0.0158)) *
              delta_t);
  odometry_x += dx;

  this->pushQueue(this->queue_dx, dx);

  double dy =
      (float)((sin(pose_msg.rotZ) * (lastOdometryReceived.twist.twist.linear.x) +   // - 0.0048) +
               cos(pose_msg.rotZ) * (lastOdometryReceived.twist.twist.linear.y)) *  // - 0.0158)) *
              delta_t);
  odometry_y += dy;

  this->pushQueue(this->queue_dy, dy);

  pose_msg.x = lastposeVisualReceived.x + odometry_x;
  pose_msg.y = lastposeVisualReceived.y + odometry_y;
  pose_msg.z = lastOdometryReceived.pose.pose.position.z;  // not received when drone not
  //   flying !!!
  if (lastOdometryReceived.pose.pose.position.z == 0)
    pose_msg.z = lastposeVisualReceived.z;

  pose_msg.header.stamp = odom_time;

  pose_msg.xvel = lastOdometryReceived.twist.twist.linear.x;
  pose_msg.yvel = lastOdometryReceived.twist.twist.linear.y;
  pose_msg.zvel =
      lastOdometryReceived.twist.twist.linear.z;  // not received when drone not flying !!!
  pose_msg.rotXvel =
      lastOdometryReceived.twist.twist.angular.x;  // not received when drone not flying !!!
  pose_msg.rotYvel =
      lastOdometryReceived.twist.twist.angular.y;  // not received when drone not flying !!!
  pose_msg.rotZvel =
      lastOdometryReceived.twist.twist.angular.z;  // not received when drone not flying !!!

  return true;
}

void PoseEstimator::pushQueue(std::queue< std::vector< double > >& myqueue, double item)
{
  std::vector< double > v(2);
  v[0] = lastOdometryReceived.header.stamp.toSec();
  v[1] = item;
  myqueue.push(v);
  if (myqueue.size() > 200)
  {
    myqueue.pop();
  }
}

void PoseEstimator::processQueue(std::queue< std::vector< double > >& myqueue, double& result)
{
  bool test = true;
  while (test && !myqueue.empty())
  {
    std::vector< double > popped = myqueue.front();
    myqueue.pop();
    test = popped[0] < lastposeVisualReceived.header.stamp.toSec();
  }

  while (!myqueue.empty())
  {
    std::vector< double > popped = myqueue.front();
    myqueue.pop();
    result += popped[1];
  }
}

int main(int argc, char** argv)
{
  // if (ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Debug))
  // {
  //   ros::console::notifyLoggerLevelsChanged();
  // }
  ROS_INFO_STREAM("pose_estimation started!");

  ros::init(argc, argv, "pose_estimation");
  PoseEstimator myPose;
  ros::Rate r(20);

  ROS_DEBUG("pose estimation initialized");
  while (!myPose.odometry_publishing && ros::ok())
  {
    ros::spinOnce();
    r.sleep();
  }

  ROS_DEBUG("ardrone_driver publishing: ending hibernate");
  myPose.doFlatTrim();
  myPose.doReset();
  ROS_DEBUG("reset done");

  while (ros::ok())
  {
    TIC(pose);
    myPose.publish_pose();
    TOC(pose, "pose");

    if (myPose.pending_reset)
    {
      ros::Time t = ros::Time::now() + ros::Duration(3);
      while (ros::Time::now() < t)
      {
        ros::spinOnce();
        myPose.doReset();
      }
      myPose.publish_end_reset_pose();
    }

    ros::spinOnce();
    r.sleep();
  }
  return 0;
}

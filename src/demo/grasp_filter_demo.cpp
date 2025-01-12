/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2014, University of Colorado, Boulder
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Univ of CO, Boulder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Dave Coleman
   Desc:   Tests the grasp generator filter
*/

// ROS
#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/pose_array.h>
#include <sensor_msgs/msg/joint_state.h>
#include <Eigen/Core>
#include <Eigen/Geometry>

// MoveIt
#include <moveit_msgs/action/move_group.h>
#include <moveit_msgs/msg/display_trajectory.h>
#include <moveit_msgs/msg/robot_state.h>
#include <moveit/kinematic_constraints/utils.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit/robot_state/conversions.h>
#include <moveit/robot_model_loader/robot_model_loader.h>

// Rviz
#include <visualization_msgs/msg/marker.h>
#include <visualization_msgs/msg/marker_array.h>

// Grasp
#include <moveit_grasps/two_finger_grasp_generator.h>
#include <moveit_grasps/two_finger_grasp_filter.h>
#include <moveit_visual_tools/moveit_visual_tools.h>
#include <moveit_grasps/two_finger_grasp_data.h>

// Parameter loading
#include <rosparam_shortcuts/rosparam_shortcuts.h>

namespace moveit_grasps_demo
{
constexpr double BLOCK_SIZE = 0.04;
const std::string LOGNAME = "grasp_filter_demo";

class GraspFilterDemo
{
public:
  // Constructor
  GraspFilterDemo()
  {
    rclcpp::NodeOptions node_options;
    node_options.automatically_declare_parameters_from_overrides(true);
    nh_ = rclcpp::Node::make_shared("grasp_test", node_options);
    // Get arm info from param server
    const std::string parent_name = "grasp_filter_demo";  // for namespacing logging messages
    rosparam_shortcuts::get(nh_, "planning_group_name", planning_group_name_);
    rosparam_shortcuts::get(nh_, "ee_group_name", ee_group_name_);

    RCLCPP_INFO_STREAM(rclcpp::get_logger("test"), "End Effector: " << ee_group_name_);
    RCLCPP_INFO_STREAM(rclcpp::get_logger("test"), "Planning Group: " << planning_group_name_);

    // ---------------------------------------------------------------------------------------------
    // Load planning scene to share
    planning_scene_monitor_ = std::make_shared<planning_scene_monitor::PlanningSceneMonitor>(nh_, "robot_description");
    if (planning_scene_monitor_->getPlanningScene())
    {
      planning_scene_monitor_->startPublishingPlanningScene(planning_scene_monitor::PlanningSceneMonitor::UPDATE_SCENE,
                                                            "grasping_planning_scene");
      planning_scene_monitor_->getPlanningScene()->setName("grasping_planning_scene");
    }
    else
    {
      RCLCPP_ERROR_STREAM(rclcpp::get_logger("test"), "Planning scene not configured");
      return;
    }

    const moveit::core::RobotModelConstPtr robot_model = planning_scene_monitor_->getRobotModel();
    arm_jmg_ = robot_model->getJointModelGroup(planning_group_name_);

    // ---------------------------------------------------------------------------------------------
    // Load the Robot Viz Tools for publishing to Rviz
    visual_tools_ = std::make_shared<moveit_visual_tools::MoveItVisualTools>(
        nh_, robot_model->getModelFrame(), "/rviz_visual_tools", planning_scene_monitor_);
    visual_tools_->loadMarkerPub();
    visual_tools_->loadRobotStatePub("/display_robot_state");
    visual_tools_->loadTrajectoryPub("/display_planned_path");
    visual_tools_->loadSharedRobotState();
    visual_tools_->enableBatchPublishing();
    visual_tools_->deleteAllMarkers();
    visual_tools_->removeAllCollisionObjects();
    visual_tools_->hideRobot();
    visual_tools_->trigger();

    moveit::core::RobotStatePtr robot_state = visual_tools_->getSharedRobotState();

    // ---------------------------------------------------------------------------------------------
    // Load grasp data specific to our robot
    grasp_data_ =
        std::make_shared<moveit_grasps::TwoFingerGraspData>(nh_, ee_group_name_, visual_tools_->getRobotModel());
    if (!grasp_data_->loadGraspData(nh_, ee_group_name_))
    {
      RCLCPP_ERROR_STREAM(rclcpp::get_logger(LOGNAME), "Failed to load Grasp Data parameters.");
      exit(-1);
    }

    // ---------------------------------------------------------------------------------------------
    // Clear out old collision objects
    visual_tools_->removeAllCollisionObjects();

    // ---------------------------------------------------------------------------------------------
    // Load grasp generator
    grasp_generator_ = std::make_shared<moveit_grasps::TwoFingerGraspGenerator>(nh_, visual_tools_);

    // Set the ideal grasp orientation for scoring
    std::vector<double> ideal_grasp_rpy = { 3.14, 0.0, 0.0 };
    grasp_generator_->setIdealTCPGraspPoseRPY(ideal_grasp_rpy);

    // We set custom grasp score weights
    moveit_grasps::TwoFingerGraspScoreWeightsPtr grasp_score_weights =
        std::make_shared<moveit_grasps::TwoFingerGraspScoreWeights>();
    grasp_score_weights->orientation_x_score_weight_ = 2.0;
    grasp_score_weights->orientation_y_score_weight_ = 2.0;
    grasp_score_weights->orientation_z_score_weight_ = 2.0;
    grasp_score_weights->translation_x_score_weight_ = 1.0;
    grasp_score_weights->translation_y_score_weight_ = 1.0;
    grasp_score_weights->translation_z_score_weight_ = 1.0;
    // Finger gripper specific weights. (Note that we do not need to set the suction gripper specific weights for our
    // finger gripper)
    grasp_score_weights->depth_score_weight_ = 2.0;
    grasp_score_weights->width_score_weight_ = 2.0;
    grasp_generator_->setGraspScoreWeights(grasp_score_weights);

    // ---------------------------------------------------------------------------------------------
    // Load grasp filter
    grasp_filter_ = std::make_shared<moveit_grasps::TwoFingerGraspFilter>(nh_, robot_state, visual_tools_);

    // ---------------------------------------------------------------------------------------------
    // Clear Markers
    visual_tools_->deleteAllMarkers();
    Eigen::Isometry3d world_cs = Eigen::Isometry3d::Identity();
    visual_tools_->publishAxis(world_cs);
  }

  bool testRandomGrasps(std::size_t num_tests)
  {
    // Generate grasps for a bunch of random objects

    // Loop
    for (std::size_t i = 0; i < num_tests; ++i)
    {
      if (!rclcpp::ok())
        break;

      RCLCPP_INFO_STREAM(rclcpp::get_logger("test"), "Adding random object " << i + 1 << " of " << num_tests);

      // Generate random cuboid
      geometry_msgs::msg::Pose object_pose;
      double xmin = 0.27;
      double xmax = 0.29;
      double ymin = -0.19;
      double ymax = -0.21;
      double zmin = 0.49;
      double zmax = 0.51;
      rviz_visual_tools::RandomPoseBounds pose_bounds(xmin, xmax, ymin, ymax, zmin, zmax);

      double cuboid_size_min = 0.01;
      double cuboid_size_max = 0.0125;
      rviz_visual_tools::RandomCuboidBounds cuboid_bounds(cuboid_size_min, cuboid_size_max);

      double depth;
      double width;
      double height;
      visual_tools_->generateRandomCuboid(object_pose, depth, width, height, pose_bounds, cuboid_bounds);
      visual_tools_->publishCuboid(object_pose, depth, width, height, rviz_visual_tools::TRANSLUCENT_DARK);
      visual_tools_->publishAxis(object_pose, rviz_visual_tools::MEDIUM);
      visual_tools_->trigger();

      // Generate set of grasps for one object
      RCLCPP_INFO_STREAM(rclcpp::get_logger("test"), "Generating cuboid grasps");
      std::vector<moveit_grasps::GraspCandidatePtr> grasp_candidates;

      // Configure the desired types of grasps
      moveit_grasps::TwoFingerGraspCandidateConfig grasp_generator_config =
          moveit_grasps::TwoFingerGraspCandidateConfig();
      grasp_generator_config.disableAll();
      grasp_generator_config.enable_face_grasps_ = true;
      grasp_generator_config.generate_y_axis_grasps_ = true;
      grasp_generator_config.generate_x_axis_grasps_ = true;
      grasp_generator_config.generate_z_axis_grasps_ = true;

      grasp_generator_->setGraspCandidateConfig(grasp_generator_config);
      grasp_generator_->generateGrasps(visual_tools_->convertPose(object_pose), depth, width, height, grasp_data_,
                                       grasp_candidates);

      // add grasps at variable depth
      // grasp_generator_->addVariableDepthGrasps(visual_tools_->convertPose(object_pose), grasp_data_,
      // grasp_candidates);

      // Filter the grasp for only the ones that are reachable
      RCLCPP_INFO_STREAM(rclcpp::get_logger("test"), "Filtering grasps kinematically");
      bool filter_pregrasps = true;
      // int direction = 1;

      // world X goes into shelf, so filter all grasps behind the YZ oriented plane of the object
      // Eigen::Isometry3d filter_pose = Eigen::Isometry3d::Identity();
      // filter_pose.translation() = visual_tools_->convertPose(object_pose).translation();
      // //visual_tools_->publishAxis(filter_pose);
      // grasp_filter_->clearCuttingPlanes();
      // grasp_filter_->addCuttingPlane(filter_pose, moveit_grasps::YZ, direction);

      // // can only reach the object from the front
      // filter_pose = Eigen::Isometry3d::Identity();
      // filter_pose = filter_pose * Eigen::AngleAxisd(M_PI / 2.0, Eigen::Vector3d::UnitY());
      // filter_pose.translation() = visual_tools_->convertPose(object_pose).translation();
      // //visual_tools_->publishAxis(filter_pose);
      // grasp_filter_->clearDesiredGraspOrientations();
      // grasp_filter_->addDesiredGraspOrientation(filter_pose, M_PI / 4.0);

      std::size_t valid_grasps = grasp_filter_->filterGrasps(grasp_candidates, planning_scene_monitor_, arm_jmg_,
                                                             visual_tools_->getSharedRobotState(), filter_pregrasps);

      if (valid_grasps == 0)
      {
        RCLCPP_ERROR_STREAM(rclcpp::get_logger("test"), "No valid grasps found after IK filtering");
        continue;
      }

      RCLCPP_INFO_STREAM(rclcpp::get_logger("test"), "finished trial, wating 5s to start next trial");
      using namespace std::chrono_literals;
      rclcpp::sleep_for(5s);  // give some time to look at results of each trial
    }                         // for each trial
    return true;
  }

private:
  // A shared node handle
  rclcpp::Node::SharedPtr nh_;

  // Tool for visualizing things in Rviz
  moveit_visual_tools::MoveItVisualToolsPtr visual_tools_;

  // Grasp generator
  moveit_grasps::TwoFingerGraspGeneratorPtr grasp_generator_;

  // Grasp filter
  moveit_grasps::TwoFingerGraspFilterPtr grasp_filter_;

  // data for generating grasps
  moveit_grasps::TwoFingerGraspDataPtr grasp_data_;

  // Shared planning scene (load once for everything)
  planning_scene_monitor::PlanningSceneMonitorPtr planning_scene_monitor_;

  // Arm
  const moveit::core::JointModelGroup* arm_jmg_;

  // Which arm should be used
  std::string ee_group_name_;
  std::string planning_group_name_;

};  // end of class

}  // namespace moveit_grasps_demo

int main(int argc, char* argv[])
{
  int num_tests = 1;

  rclcpp::init(argc, argv);
  rclcpp::NodeOptions node_options;
  node_options.automatically_declare_parameters_from_overrides(true);
  auto grasp_generator_demo_node = rclcpp::Node::make_shared("grasp_generator_demo", node_options);

  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(grasp_generator_demo_node);
  std::thread([&executor]() { executor.spin(); }).detach();

  // Seed random
  srand(grasp_generator_demo_node->get_clock()->now().seconds());

  // Benchmark time
  rclcpp::Time start_time;
  start_time = grasp_generator_demo_node->get_clock()->now();

  // Run Tests
  moveit_grasps_demo::GraspFilterDemo tester;
  tester.testRandomGrasps(num_tests);

  // Benchmark time
  double duration = (grasp_generator_demo_node->get_clock()->now() - start_time).seconds();
  RCLCPP_INFO_STREAM(rclcpp::get_logger("grasp_filter_demo"), "Total time: " << duration << "\t" << num_tests);
  std::cout << "Total time: " << duration << "\t" << num_tests << std::endl;

  using namespace std::chrono_literals;
  rclcpp::sleep_for(1s);  // let rviz markers finish publishing

  return 0;
}

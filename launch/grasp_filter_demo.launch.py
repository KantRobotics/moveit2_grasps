import os
import yaml
from launch import LaunchDescription
from launch_ros.actions import Node
from ament_index_python.packages import get_package_share_directory


def load_file(package_name, file_path):
    package_path = get_package_share_directory(package_name)
    absolute_file_path = os.path.join(package_path, file_path)

    try:
        with open(absolute_file_path, "r") as file:
            return file.read()
    except EnvironmentError:  # parent of IOError, OSError *and* WindowsError where available
        return None


def load_yaml(package_name, file_path):
    package_path = get_package_share_directory(package_name)
    absolute_file_path = os.path.join(package_path, file_path)

    try:
        with open(absolute_file_path, "r") as file:
            return yaml.safe_load(file)
    except EnvironmentError:  # parent of IOError, OSError *and* WindowsError where available
        return None


def generate_launch_description():
    # planning_context
    robot_description_config = load_file(
        "moveit_resources_panda_description", "urdf/panda.urdf"
    )
    robot_description = {"robot_description": robot_description_config}

    robot_description_semantic_config = load_file(
        "moveit_resources_panda_moveit_config", "config/panda.srdf"
    )
    robot_description_semantic = {
        "robot_description_semantic": robot_description_semantic_config
    }

    kinematics_yaml = load_yaml(
        "moveit_resources_panda_moveit_config", "config/kinematics.yaml"
    )

    gripper = {"gripper": "two_finger"}
    ee_group_name = {"ee_group_name": "hand"}
    planning_group_name = {"planning_group_name": "panda_arm"}
    statistics_verbose = {"moveit_grasps/filter/statistics_verbose": True}
    show_filtered_grasps = {"moveit_grasps/filter/show_filtered_grasps": True}
    show_cutting_planes = {"moveit_grasps/filter/show_cutting_planes": True}
    show_grasp_filter_collision_if_failed = {"moveit_grasps/filter/show_grasp_filter_collision_if_failed": True}
    show_filtered_arm_solutions = {"moveit_grasps/filter/show_filtered_arm_solutions": True}
    panda_grasp_data_yaml = load_yaml(
        "moveit_grasps", "config_robot/panda_grasp_data.yaml"
    )
    moveit_grasps_config_yaml = load_yaml(
        "moveit_grasps", "config/moveit_grasps_config.yaml"
    )

    # MoveGroupInterface demo executable
    grasps_demo = Node(
        name="moveit_generator_demo",
        package="moveit_grasps",
        executable="moveit_grasps_grasp_filter_demo",
        output="screen",
        parameters=[
            robot_description,
            robot_description_semantic,
            kinematics_yaml,
            gripper,
            ee_group_name,
            planning_group_name,
            statistics_verbose,
            show_filtered_grasps,
            show_cutting_planes,
            show_grasp_filter_collision_if_failed,
            show_filtered_arm_solutions,
            panda_grasp_data_yaml,
            moveit_grasps_config_yaml,
        ],
    )

    return LaunchDescription([grasps_demo])

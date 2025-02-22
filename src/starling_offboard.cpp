#include "starling_offboard_cpp/starling_offboard.hpp"

StarlingOffboard::StarlingOffboard() : Node("starling_offboard"), qos_(1) {
  GetNodeParameters();
  // QoS
  rmw_qos_profile_t qos_profile = rmw_qos_profile_sensor_data;
  qos_ = rclcpp::QoS(
      rclcpp::QoSInitialization(qos_profile.history, params_.buffer_size),
      qos_profile);

  InitializeSubscribers();
  InitializePublishers();

  clock_ = std::make_shared<rclcpp::Clock>();

  takeoff_pos_ << params_.x_takeoff, params_.y_takeoff, params_.z_takeoff, 1.0;

  // Transformation matrix from Mission to NED
  // TODO revert x,y flip
  /*R_z = Eigen::AngleAxisd(3.*M_PI/2. - heading_,
  Eigen::Vector3d::UnitZ()).toRotationMatrix(); R_x = Eigen::AngleAxisd(M_PI,
  Eigen::Vector3d::UnitX()).toRotationMatrix();

  // TODO currently not using this
  rot_mat_ = R_x * R_z;

  T_ned_miss_.block<3,3>(0,0) = rot_mat_;
  T_miss_ned_.block<3,3>(0,0) = T_ned_miss_.block<3,3>(0,0).transpose();
  */
  // TODO Revert before real flight
  // 10Hz Timer
}

void StarlingOffboard::GetNodeParameters() {
  this->declare_parameter("buffer_size", 5);
  this->get_parameter("buffer_size", params_.buffer_size);

  this->declare_parameter<int>("robot_id", 1);
  this->get_parameter("robot_id", params_.robot_id);

  this->declare_parameter<double>("position_tolerance", 1.0);
  this->get_parameter("position_tolerance", params_.position_tolerance);

  this->declare_parameter<double>("env_scale_factor", 1.0);
  this->get_parameter("env_scale_factor", params_.env_scale_factor);

  this->declare_parameter<double>("x_takeoff", 0.0);
  this->get_parameter("x_takeoff", params_.x_takeoff);

  this->declare_parameter<double>("y_takeoff", 0.0);
  this->get_parameter("y_takeoff", params_.y_takeoff);

  this->declare_parameter<double>("alt", 2.0);
  this->get_parameter("alt", params_.z_takeoff);

  this->declare_parameter<double>("yaw", 2.4);
  this->get_parameter("yaw", params_.yaw);

  this->declare_parameter<double>("max_speed", 2.0);
  this->get_parameter("max_speed", params_.max_speed);
}

void StarlingOffboard::InitializeSubscribers() {
  subs_.vehicle_status =
      this->create_subscription<px4_msgs::msg::VehicleStatus>(
          "fmu/out/vehicle_status", qos_,
          [this](const px4_msgs::msg::VehicleStatus::UniquePtr msg) {
            arming_state_ = msg->arming_state;
          });

  // Used if we need to bypass homify for whatever reason
  subs_.vehicle_gps_pos = this->create_subscription<px4_msgs::msg::SensorGps>(
      "fmu/out/vehicle_gps_position", qos_,
      [this](const px4_msgs::msg::SensorGps::UniquePtr msg) {
        gps_pos_msg_ = *msg;
        //    launch_gps_lat_ = ConvertRawGPSToDegrees(gps_pos_msg_.lat);
        //    launch_gps_lon_ = ConvertRawGPSToDegrees(gps_pos_msg_.lon);
        //       gps_received_ = true;
      });

  subs_.mission_origin_gps =
      this->create_subscription<geometry_msgs::msg::Point>(
          "/pac_gcs/mission_origin_gps", qos_,
          [this](const geometry_msgs::msg::Point::UniquePtr msg) {
            // RCLCPP_INFO(this->get_logger(), "Origin GPS received");
            mission_origin_lon_ = msg->x;
            mission_origin_lat_ = msg->y;
            heading_ = msg->z;
            origin_gps_received_ = true;
          });

  subs_.launch_gps = this->create_subscription<geometry_msgs::msg::Point>(
      "launch_gps", qos_,
      [this](const geometry_msgs::msg::Point::UniquePtr msg) {
        // RCLCPP_INFO(this->get_logger(), "Launch GPS received");
        launch_gps_lat_ = msg->x;
        launch_gps_lon_ = msg->y;
        gps_received_ = true;
      });

  // Velocity Translation (TwistStamped [GNN] to TrajectorySetpoint [PX4])
  subs_.cmd_vel = this->create_subscription<geometry_msgs::msg::TwistStamped>(
      "cmd_vel", qos_,
      std::bind(&StarlingOffboard::UpdateVel, this, std::placeholders::_1));

  // Position Translation (VehicleLocalPosition [PX4] to PoseStamped [GNN])
  subs_.vehicle_local_pos =
      this->create_subscription<px4_msgs::msg::VehicleLocalPosition>(
          "fmu/out/vehicle_local_position", qos_,
          std::bind(&StarlingOffboard::VehicleLocalPosCallback, this,
                    std::placeholders::_1));
}

void StarlingOffboard::InitializePublishers() {
  pubs_.pose =
      this->create_publisher<geometry_msgs::msg::PoseStamped>("pose", qos_);
  auto qos_reliable = rclcpp::QoS(rclcpp::QoSInitialization(
      rmw_qos_profile_default.history, params_.buffer_size));
  qos_reliable.reliability(RMW_QOS_POLICY_RELIABILITY_RELIABLE);

  // High BW
  pubs_.nav_path =
      this->create_publisher<nav_msgs::msg::Path>("path", qos_reliable);

  pubs_.drone_status =
      this->create_publisher<std_msgs::msg::String>("drone_status", qos_);

  pubs_.offboard_control_mode =
      this->create_publisher<px4_msgs::msg::OffboardControlMode>(
          "fmu/in/offboard_control_mode", params_.buffer_size);
  pubs_.vehicle_command = this->create_publisher<VehicleCommand>(
      "fmu/in/vehicle_command", params_.buffer_size);
  pubs_.traj_setpoint =
      this->create_publisher<px4_msgs::msg::TrajectorySetpoint>(
          "fmu/in/trajectory_setpoint", params_.buffer_size);
  timer_ = this->create_wall_timer(
      100ms, std::bind(&StarlingOffboard::TimerCallback, this));
  path_pub_timer_ = this->create_wall_timer(
      1000ms, std::bind(&StarlingOffboard::PathPublisherTimerCallback, this));
}

/**
 * @brief Main Loop
 */
void StarlingOffboard::TimerCallback() {
  // std::cout << "State: " << state_ << std::endl;
  //  Publish the current state
  auto state_msg = std_msgs::msg::String();
  state_msg.data = StateToString(state_);
  pubs_.drone_status->publish(state_msg);

  // State Machine
  switch (state_) {
    case State::IDLE:
      RCLCPP_INFO(this->get_logger(), "State: idle");

      if (gps_received_ && origin_gps_received_) {
        // Global origin
        RCLCPP_INFO(this->get_logger(), "Global origin");
        RCLCPP_INFO(this->get_logger(), "lat: %.8f", mission_origin_lat_);
        RCLCPP_INFO(this->get_logger(), "lon: %.8f", mission_origin_lon_);
        RCLCPP_INFO(this->get_logger(), "heading: %.8f", heading_);

        // Global startup location
        RCLCPP_INFO(this->get_logger(), "Global received");
        RCLCPP_INFO(this->get_logger(), "lat: %.8f", launch_gps_lat_);
        RCLCPP_INFO(this->get_logger(), "lon: %.8f", launch_gps_lon_);

        // Compute the translation from the home position to the current (start
        // up position)
        double distance;
        double azimuth_origin_to_target;
        double azimuth_target_to_origin;

        const GeographicLib::Geodesic geod = GeographicLib::Geodesic::WGS84();
        geod.Inverse(mission_origin_lat_, mission_origin_lon_, launch_gps_lat_,
                     launch_gps_lon_, distance, azimuth_origin_to_target,
                     azimuth_target_to_origin);

        RCLCPP_INFO(this->get_logger(), "Distance to origin: %f", distance);
        RCLCPP_INFO(this->get_logger(), "Azimuth origin to target: %f",
                    azimuth_origin_to_target);
        RCLCPP_INFO(this->get_logger(), "Azimuth target to origin: %f",
                    azimuth_target_to_origin);

        double x = distance * cos(azimuth_origin_to_target * M_PI / 180.0);
        double y = distance * sin(azimuth_origin_to_target * M_PI / 180.0);
        double z = 0.0;

        Eigen::Matrix3d rot_mat_z = Eigen::AngleAxisd(3. * M_PI / 2. - heading_,
                                                      Eigen::Vector3d::UnitZ())
                                        .toRotationMatrix();
        Eigen::Matrix3d rot_mat_x =
            Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitX())
                .toRotationMatrix();

        Eigen::Matrix3d rot_mat = rot_mat_x * rot_mat_z;

        T_ned_miss_.block<3, 3>(0, 0) = rot_mat;
        T_miss_ned_.block<3, 3>(0, 0) =
            T_ned_miss_.block<3, 3>(0, 0).transpose();

        Eigen::Vector3d translation = Eigen::Vector3d(x, y, z);
        T_miss_ned_.block<3, 1>(0, 3) = -translation;

        T_ned_miss_ = T_miss_ned_.inverse();

        Eigen::Vector4d current_mission_pos = TransformVec(
            Eigen::Vector4d(pos_msg_.x, pos_msg_.y, pos_msg_.z, 1.0),
            T_ned_miss_);
        RCLCPP_INFO(this->get_logger(), "Current mission pos: %f, %f, %f",
                    current_mission_pos[0], current_mission_pos[1],
                    current_mission_pos[2]);

        assert((T_miss_ned_ * T_ned_miss_)
                   .isApprox(Eigen::Matrix4d::Identity(), 0.001));
        takeoff_pos_[0] = current_mission_pos[0];
        takeoff_pos_[1] = current_mission_pos[1];

        takeoff_pos_ned_ = TransformVec(takeoff_pos_, T_miss_ned_);

        RCLCPP_INFO(this->get_logger(), "Translation: %f, %f, %f", x, y, z);
        RCLCPP_INFO(this->get_logger(), "Takeoff pos (miss): %f, %f, %f",
                    takeoff_pos_[0], takeoff_pos_[1], takeoff_pos_[2]);
        RCLCPP_INFO(this->get_logger(), "Takeoff pos (ned): %f, %f, %f",
                    takeoff_pos_ned_[0], takeoff_pos_ned_[1],
                    takeoff_pos_ned_[2]);

        Eigen::Vector4d takeoff_pos_check =
            TransformVec(takeoff_pos_ned_, T_ned_miss_);
        RCLCPP_INFO(this->get_logger(), "Takeoff pos check: %f, %f, %f",
                    takeoff_pos_check[0], takeoff_pos_check[1],
                    takeoff_pos_check[2]);

        RCLCPP_INFO(this->get_logger(), "T_miss_ned:\n%s", EigenMatToStr(T_miss_ned_).c_str());
        RCLCPP_INFO(this->get_logger(), "T_ned_miss:\n%s", EigenMatToStr(T_ned_miss_).c_str());

        assert(TransformVec(takeoff_pos_ned_, T_ned_miss_)
                   .isApprox(takeoff_pos_, 0.1));

        state_ = State::ARMING;
        RCLCPP_INFO(this->get_logger(), "State: arming");
      }
      break;

    case State::ARMING:
      if (offboard_setpoint_counter_ == 10) {
        // Change to Offboard mode after 10 setpoints
        this->PubVehicleCommand(VehicleCommand::VEHICLE_CMD_DO_SET_MODE, 1, 6);

        // Arm the vehicle
        this->Arm();

        if (arming_state_ == 2) {
          RCLCPP_INFO(this->get_logger(), "Vehicle armed");
          state_ = State::TAKEOFF;
          std::cout << "State: " << state_ << std::endl;
        } else {
          RCLCPP_INFO(this->get_logger(), "Vehicle not armed");
          // Retry
          offboard_setpoint_counter_ = 0;
        }
      }

      // Send 10 setpoints before attempting to Arm
      // offboard_control_mode needs to be paired with trajectory_setpoint
      PubOffboardControlMode(true, false);
      PubTrajSetpointPos(takeoff_pos_ned_);

      // stop the counter after reaching 11
      if (offboard_setpoint_counter_ < 11) {
        offboard_setpoint_counter_++;
      }
      break;

    case State::LANDING:
    case State::TAKEOFF:

      // error calculation
      takeoff_completed_ = this->HasReachedPos(takeoff_pos_ned_);

      if (takeoff_completed_) {
        PubOffboardControlMode(true, false);
        PubTrajSetpointVel(stop_vel_);
        state_ = State::MISSION;
        std::cout << "State: " << state_ << std::endl;
        RCLCPP_INFO(this->get_logger(),
                    "Takeoff complete -- reached setpoint within TOL");
      } else {
        // offboard_control_mode needs to be paired with trajectory_setpoint
        PubOffboardControlMode(true, false);
        PubTrajSetpointPos(takeoff_pos_ned_);
      }

      break;

    // GNN, Square, etc..
    case State::MISSION:

      // offboard_control_mode needs to be paired with trajectory_setpoint
      // If the message rate drops bellow 2Hz, the drone exits offboard control
      PubOffboardControlMode(false, true);

      rclcpp::Time time_now = clock_->now();

      rclcpp::Duration duration = time_now - time_last_vel_update_;
      if (duration.seconds() > 1.0) {
        double err_z = (pos_msg_.z + params_.z_takeoff);
        stop_vel_[2] = -1.0 * err_z;
        ClampVelocity(stop_vel_);
        PubTrajSetpointVel(stop_vel_);
        RCLCPP_INFO(this->get_logger(), "Mission velocity update timeout; stop velocity (%f, %f, %f)", stop_vel_[0], stop_vel_[1], stop_vel_[2]);
      }

      else {
        //RCLCPP_INFO(this->get_logger(), "NED Velocity (%f, %f, %f)", vel_ned_[0], vel_ned_[1], vel_ned_[2]);
        PubTrajSetpointVel(vel_ned_);
        time_last_vel_update_ = time_now;
      }
      break;
  }
}

void StarlingOffboard::PathPublisherTimerCallback() {
  // Publish the path
  path_.header.stamp = this->get_clock()->now();
  path_.header.frame_id = "map";

  if (path_.poses.size() == 0) {
    return;
  }
  if (std::abs(curr_position_[0]) < 0.01 &&
      std::abs(curr_position_[1]) < 0.01) {
    return;
  }
  Eigen::Vector4d scaled_pos = curr_position_ * params_.env_scale_factor;
  geometry_msgs::msg::PoseStamped latest_pose = path_.poses.back();
  double max_dist =
      std::max(std::abs(latest_pose.pose.position.x - scaled_pos[0]),
               std::abs(latest_pose.pose.position.y - scaled_pos[1]));
  max_dist =
      std::max(max_dist, std::abs(latest_pose.pose.position.z - scaled_pos[2]));
  if (max_dist < 1) {
    pubs_.nav_path->publish(path_);
    return;
  }
  latest_pose.header.stamp = this->get_clock()->now();
  latest_pose.header.frame_id = "map";

  latest_pose.pose.position.x = scaled_pos[0];
  latest_pose.pose.position.y = scaled_pos[1];
  latest_pose.pose.position.z = scaled_pos[2];
  path_.poses.push_back(latest_pose);
  pubs_.nav_path->publish(path_);
}

/**
 * @brief Compute the velocity to reach the target position
 */
Eigen::Vector4d StarlingOffboard::ComputeVel(
    const Eigen::Vector4d& target_pos) {
  const double kP = 1.0;

  const double err_x = (pos_msg_.x - target_pos[0]);
  const double err_y = (pos_msg_.y - target_pos[1]);
  const double err_z = (pos_msg_.z - target_pos[2]);

  Eigen::Vector4d vel;
  vel << -kP * err_x, -kP * err_y, -kP * err_z, 0.0;
  return vel;
}

/**
 * @brief Check if the drone has reached the target position within tolerance
 */
bool StarlingOffboard::HasReachedPos(const Eigen::Vector4d& target_pos) {
  const double err_x = std::abs(pos_msg_.x - target_pos[0]);
  const double err_y = std::abs(pos_msg_.y - target_pos[1]);
  const double err_z = std::abs(pos_msg_.z - target_pos[2]);

  return err_x < params_.position_tolerance &&
         err_y < params_.position_tolerance &&
         err_z < params_.position_tolerance;
}

/**
 * @brief Send a command to Arm the vehicle
 */
void StarlingOffboard::Arm() {
  PubVehicleCommand(VehicleCommand::VEHICLE_CMD_COMPONENT_ARM_DISARM, 1.0, 0.0,
                    0.0, 0.0, 0.0);
  RCLCPP_INFO(this->get_logger(), "Arm command send");
}

/**
 * @brief Send a command to Disarm the vehicle
 */
void StarlingOffboard::Disarm() {
  PubVehicleCommand(VehicleCommand::VEHICLE_CMD_COMPONENT_ARM_DISARM, 0.0, 0.0,
                    0.0, 0.0, 0.0);
  RCLCPP_INFO(this->get_logger(), "Disarm command send");
}

/**
 * @brief Publish the offboard control mode.
 *        For this example, only position and altitude controls are active.
 */
void StarlingOffboard::PubOffboardControlMode(const bool is_pos,
                                              const bool is_vel) {
  px4_msgs::msg::OffboardControlMode msg{};
  msg.position = is_pos;
  msg.velocity = is_vel;
  msg.acceleration = false;
  msg.attitude = false;
  msg.body_rate = false;
  msg.timestamp = this->get_clock()->now().nanoseconds() / 1000;
  pubs_.offboard_control_mode->publish(msg);
}

/**
 * @brief Publish a trajectory setpoint (vel)
 */
void StarlingOffboard::PubTrajSetpointVel(const Eigen::Vector4d& target_vel) {
  TrajectorySetpoint msg{};
  msg.position = {std::nanf(""), std::nanf(""),
                  std::nanf("")};  // required for vel control in px4
  msg.velocity = {static_cast<float>(target_vel[0]),
                 static_cast<float>(target_vel[1]),
                 static_cast<float>(target_vel[2])};
  msg.yaw = static_cast<float>(params_.yaw);  // [-PI:PI]
  msg.timestamp = this->get_clock()->now().nanoseconds() / 1000;
  pubs_.traj_setpoint->publish(msg);
}

/**
 * @brief Publish a trajectory setpoint (position)
 */
void StarlingOffboard::PubTrajSetpointPos(const Eigen::Vector4d& target_pos) {
  TrajectorySetpoint msg{};
  msg.position = {static_cast<float>(target_pos[0]),
                 static_cast<float>(target_pos[1]),
                 static_cast<float>(target_pos[2])};
  msg.yaw = static_cast<float>(params_.yaw);  // [-PI:PI]
  msg.timestamp = this->get_clock()->now().nanoseconds() / 1000;
  pubs_.traj_setpoint->publish(msg);
}

/**
 * @brief Publish the trajectory setpoint (TwistStamped) to the PX4
 */
void StarlingOffboard::UpdateVel(
    const geometry_msgs::msg::TwistStamped::SharedPtr cmd_vel) {
  // Proportional controller to maintain altitude
  const double kP = 1.0;
  const double err_z = (params_.z_takeoff + pos_msg_.z);

  // const Eigen::Vector4d vel_mission (
  //                           (double) clamp(scale *
  //                           cmd_vel->twist.linear.y, -2.0, 2.0), (double)
  //                           clamp(scale * cmd_vel->twist.linear.x,
  //                           -2.0, 2.0), (double) clamp((double)(-kP * err_z),
  //                           -2.0, 2.0), 1.0);

  const Eigen::Vector4d vel_mission(cmd_vel->twist.linear.x,
                                    cmd_vel->twist.linear.y, kP * err_z, 0.0);

  // Transform the velocity from the mission frame to NED
  vel_ned_ = TransformVec(vel_mission, T_miss_ned_);
  ClampVelocity(vel_ned_);
  time_last_vel_update_ = clock_->now();
}

/**
 * @brief Publish the pose (PoseStamped) to the GNN. Publishing the path as well
 * for visualization
 */
void StarlingOffboard::VehicleLocalPosCallback(
    const px4_msgs::msg::VehicleLocalPosition::SharedPtr
        pos_msg) {
  pos_msg_ = *pos_msg;
  const Eigen::Vector4d pos_vec(pos_msg->x, pos_msg->y, pos_msg->z, 1.0);

  const Eigen::Vector4d vehicle_mission_position =
      TransformVec(pos_vec, T_ned_miss_);
  curr_position_ = vehicle_mission_position;
  if (path_.poses.size() == 0) {
    if (std::abs(curr_position_[0]) < 1. && std::abs(curr_position_[1]) < 1.) {
      return;
    }
    if (curr_position_[0] != 0 && curr_position_[1] != 0 &&
        curr_position_[2] != 0) {
      geometry_msgs::msg::PoseStamped gnn_pose;
      gnn_pose.header.stamp = this->get_clock()->now();
      gnn_pose.header.frame_id = "map";
      gnn_pose.pose.position.x = vehicle_mission_position[0];
      gnn_pose.pose.position.y = vehicle_mission_position[1];
      gnn_pose.pose.position.z = vehicle_mission_position[2];
      path_.poses.push_back(gnn_pose);
    }
  }

  // Publish the current pose
  geometry_msgs::msg::PoseStamped gnn_pose;
  gnn_pose.header.stamp = this->get_clock()->now();
  gnn_pose.header.frame_id = "map";

  // TODO
  gnn_pose.pose.position.x = vehicle_mission_position[0];
  gnn_pose.pose.position.y = vehicle_mission_position[1];
  gnn_pose.pose.position.z = vehicle_mission_position[2];
  pubs_.pose->publish(gnn_pose);
}

/**
 * @brief Publish vehicle commands
 * @param command   Command code (matches VehicleCommand and MAVLink MAV_CMD
 * codes)
 * @param param1    Command parameter 1
 * @param param2    Command parameter 2
 * @param param5    Command parameter 5
 * @param param6    Command parameter 6
 * @param param7    Command parameter 7
 */
void StarlingOffboard::PubVehicleCommand(uint32_t command, double param1,
                                         double param2, double param5,
                                         double param6, double param7) {
  VehicleCommand msg{};
  msg.param1 = static_cast<float>(param1);
  msg.param2 = static_cast<float>(param2);
  msg.param5 = param5;
  msg.param6 = param6;
  msg.param7 = static_cast<float>(param7);
  msg.command = command;
  msg.target_system = static_cast<uint8_t>(params_.robot_id + 1);
  msg.target_component = 1;
  msg.source_system = 1;
  msg.source_component = 1;
  msg.from_external = true;
  msg.timestamp = static_cast<uint64_t>(this->get_clock()->now().nanoseconds() / 1000);
  pubs_.vehicle_command->publish(msg);
}

int main(int argc, char* argv[]) {
  std::cout << "Starting Starling offboard node..." << std::endl;
  setvbuf(stdout, NULL, _IONBF, BUFSIZ);
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<StarlingOffboard>());
  rclcpp::shutdown();
  return 0;
}

// Copyright 2020 Tier IV, Inc.
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

#ifndef LANE_DEPARTURE_CHECKER__LANE_DEPARTURE_CHECKER_HPP_
#define LANE_DEPARTURE_CHECKER__LANE_DEPARTURE_CHECKER_HPP_

#include <rosidl_runtime_cpp/message_initialization.hpp>
#include <tier4_autoware_utils/geometry/boost_geometry.hpp>
#include <tier4_autoware_utils/geometry/pose_deviation.hpp>
#include <vehicle_info_util/vehicle_info_util.hpp>

#include <autoware_auto_planning_msgs/msg/path_with_lane_id.hpp>
#include <autoware_auto_planning_msgs/msg/trajectory.hpp>
#include <autoware_auto_planning_msgs/msg/trajectory_point.hpp>
#include <autoware_planning_msgs/msg/lanelet_route.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>
#include <geometry_msgs/msg/twist_stamped.hpp>
#include <nav_msgs/msg/odometry.hpp>

#include <boost/geometry/algorithms/envelope.hpp>
#include <boost/geometry/algorithms/union.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <lanelet2_core/LaneletMap.h>
#include <lanelet2_core/geometry/BoundingBox.h>
#include <lanelet2_core/geometry/LaneletMap.h>
#include <lanelet2_core/geometry/LineString.h>
#include <lanelet2_core/geometry/Polygon.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lane_departure_checker
{
using autoware_auto_planning_msgs::msg::PathWithLaneId;
using autoware_auto_planning_msgs::msg::Trajectory;
using autoware_auto_planning_msgs::msg::TrajectoryPoint;
using autoware_planning_msgs::msg::LaneletRoute;
using tier4_autoware_utils::LinearRing2d;
using tier4_autoware_utils::PoseDeviation;
using tier4_autoware_utils::Segment2d;
using TrajectoryPoints = std::vector<TrajectoryPoint>;
typedef boost::geometry::index::rtree<Segment2d, boost::geometry::index::rstar<16>> SegmentRtree;

struct Param
{
  double footprint_margin_scale;
  double resample_interval;
  double max_deceleration;
  double delay_time;
  double max_lateral_deviation;
  double max_longitudinal_deviation;
  double max_yaw_deviation_deg;
  double min_braking_distance;
  // nearest search to ego
  double ego_nearest_dist_threshold;
  double ego_nearest_yaw_threshold;
};

struct Input
{
  nav_msgs::msg::Odometry::ConstSharedPtr current_odom{};
  lanelet::LaneletMapPtr lanelet_map{};
  LaneletRoute::ConstSharedPtr route{};
  lanelet::ConstLanelets route_lanelets{};
  lanelet::ConstLanelets shoulder_lanelets{};
  Trajectory::ConstSharedPtr reference_trajectory{};
  Trajectory::ConstSharedPtr predicted_trajectory{};
  std::vector<std::string> boundary_types_to_detect{};
};

struct Output
{
  std::map<std::string, double> processing_time_map{};
  bool will_leave_lane{};
  bool is_out_of_lane{};
  bool will_cross_boundary{};
  PoseDeviation trajectory_deviation{};
  lanelet::ConstLanelets candidate_lanelets{};
  TrajectoryPoints resampled_trajectory{};
  std::vector<LinearRing2d> vehicle_footprints{};
  std::vector<LinearRing2d> vehicle_passing_areas{};
};

class LaneDepartureChecker
{
public:
  Output update(const Input & input);

  void setParam(const Param & param, const vehicle_info_util::VehicleInfo vehicle_info)
  {
    param_ = param;
    vehicle_info_ptr_ = std::make_shared<vehicle_info_util::VehicleInfo>(vehicle_info);
  }

  void setParam(const Param & param) { param_ = param; }

  void setVehicleInfo(const vehicle_info_util::VehicleInfo vehicle_info)
  {
    vehicle_info_ptr_ = std::make_shared<vehicle_info_util::VehicleInfo>(vehicle_info);
  }

  bool checkPathWillLeaveLane(
    const lanelet::ConstLanelets & lanelets, const PathWithLaneId & path) const;

  std::vector<std::pair<double, lanelet::Lanelet>> getLaneletsFromPath(
    const lanelet::LaneletMapPtr lanelet_map_ptr, const PathWithLaneId & path) const;

  std::optional<lanelet::BasicPolygon2d> getFusedLaneletPolygonForPath(
    const lanelet::LaneletMapPtr lanelet_map_ptr, const PathWithLaneId & path) const;

  bool checkPathWillLeaveLane(
    const lanelet::LaneletMapPtr lanelet_map_ptr, const PathWithLaneId & path) const;

  PathWithLaneId cropPointsOutsideOfLanes(
    const lanelet::LaneletMapPtr lanelet_map_ptr, const PathWithLaneId & path,
    const size_t end_index);

  static bool isOutOfLane(
    const lanelet::ConstLanelets & candidate_lanelets, const LinearRing2d & vehicle_footprint);

private:
  Param param_;
  std::shared_ptr<vehicle_info_util::VehicleInfo> vehicle_info_ptr_;

  static PoseDeviation calcTrajectoryDeviation(
    const Trajectory & trajectory, const geometry_msgs::msg::Pose & pose,
    const double dist_threshold, const double yaw_threshold);

  //! This function assumes the input trajectory is sampled dense enough
  static TrajectoryPoints resampleTrajectory(const Trajectory & trajectory, const double interval);

  static TrajectoryPoints cutTrajectory(const TrajectoryPoints & trajectory, const double length);

  std::vector<LinearRing2d> createVehicleFootprints(
    const geometry_msgs::msg::PoseWithCovariance & covariance, const TrajectoryPoints & trajectory,
    const Param & param);
  std::vector<LinearRing2d> createVehicleFootprints(const PathWithLaneId & path) const;

  static std::vector<LinearRing2d> createVehiclePassingAreas(
    const std::vector<LinearRing2d> & vehicle_footprints);

  static bool willLeaveLane(
    const lanelet::ConstLanelets & candidate_lanelets,
    const std::vector<LinearRing2d> & vehicle_footprints);

  double calcMaxSearchLengthForBoundaries(const Trajectory & trajectory) const;

  static SegmentRtree extractUncrossableBoundaries(
    const lanelet::LaneletMap & lanelet_map, const geometry_msgs::msg::Point & ego_point,
    const double max_search_length, const std::vector<std::string> & boundary_types_to_detect);

  static bool willCrossBoundary(
    const std::vector<LinearRing2d> & vehicle_footprints,
    const SegmentRtree & uncrossable_segments);
};
}  // namespace lane_departure_checker

#endif  // LANE_DEPARTURE_CHECKER__LANE_DEPARTURE_CHECKER_HPP_

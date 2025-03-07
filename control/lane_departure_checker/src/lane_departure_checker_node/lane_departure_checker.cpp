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

#include "lane_departure_checker/lane_departure_checker.hpp"

#include "lane_departure_checker/util/create_vehicle_footprint.hpp"

#include <motion_utils/trajectory/trajectory.hpp>
#include <tier4_autoware_utils/geometry/geometry.hpp>
#include <tier4_autoware_utils/math/normalization.hpp>
#include <tier4_autoware_utils/math/unit_conversion.hpp>
#include <tier4_autoware_utils/system/stop_watch.hpp>

#include <boost/geometry.hpp>

#include <lanelet2_core/geometry/Polygon.h>
#include <tf2/utils.h>

#include <algorithm>
#include <vector>

using motion_utils::calcArcLength;
using tier4_autoware_utils::LinearRing2d;
using tier4_autoware_utils::LineString2d;
using tier4_autoware_utils::MultiPoint2d;
using tier4_autoware_utils::MultiPolygon2d;
using tier4_autoware_utils::Point2d;

namespace
{
using autoware_auto_planning_msgs::msg::Trajectory;
using autoware_auto_planning_msgs::msg::TrajectoryPoint;
using TrajectoryPoints = std::vector<TrajectoryPoint>;
using geometry_msgs::msg::Point;

double calcBrakingDistance(
  const double abs_velocity, const double max_deceleration, const double delay_time)
{
  return (abs_velocity * abs_velocity) / (2.0 * max_deceleration) + delay_time * abs_velocity;
}

bool isInAnyLane(const lanelet::ConstLanelets & candidate_lanelets, const Point2d & point)
{
  for (const auto & ll : candidate_lanelets) {
    if (boost::geometry::within(point, ll.polygon2d().basicPolygon())) {
      return true;
    }
  }

  return false;
}

LinearRing2d createHullFromFootprints(const std::vector<LinearRing2d> & footprints)
{
  MultiPoint2d combined;
  for (const auto & footprint : footprints) {
    for (const auto & p : footprint) {
      combined.push_back(p);
    }
  }

  LinearRing2d hull;
  boost::geometry::convex_hull(combined, hull);

  return hull;
}

lanelet::ConstLanelets getCandidateLanelets(
  const lanelet::ConstLanelets & route_lanelets,
  const std::vector<LinearRing2d> & vehicle_footprints)
{
  lanelet::ConstLanelets candidate_lanelets;

  // Find lanes within the convex hull of footprints
  const auto footprint_hull = createHullFromFootprints(vehicle_footprints);
  for (const auto & route_lanelet : route_lanelets) {
    const auto poly = route_lanelet.polygon2d().basicPolygon();
    if (!boost::geometry::disjoint(poly, footprint_hull)) {
      candidate_lanelets.push_back(route_lanelet);
    }
  }

  return candidate_lanelets;
}

}  // namespace

namespace lane_departure_checker
{
Output LaneDepartureChecker::update(const Input & input)
{
  Output output{};

  tier4_autoware_utils::StopWatch<std::chrono::milliseconds> stop_watch;

  output.trajectory_deviation = calcTrajectoryDeviation(
    *input.reference_trajectory, input.current_odom->pose.pose, param_.ego_nearest_dist_threshold,
    param_.ego_nearest_yaw_threshold);
  output.processing_time_map["calcTrajectoryDeviation"] = stop_watch.toc(true);

  {
    constexpr double min_velocity = 0.01;
    const auto & raw_abs_velocity = std::abs(input.current_odom->twist.twist.linear.x);
    const auto abs_velocity = raw_abs_velocity < min_velocity ? 0.0 : raw_abs_velocity;

    const auto braking_distance = std::max(
      param_.min_braking_distance,
      calcBrakingDistance(abs_velocity, param_.max_deceleration, param_.delay_time));

    output.resampled_trajectory = cutTrajectory(
      resampleTrajectory(*input.predicted_trajectory, param_.resample_interval), braking_distance);
    output.processing_time_map["resampleTrajectory"] = stop_watch.toc(true);
  }
  output.vehicle_footprints =
    createVehicleFootprints(input.current_odom->pose, output.resampled_trajectory, param_);
  output.processing_time_map["createVehicleFootprints"] = stop_watch.toc(true);

  output.vehicle_passing_areas = createVehiclePassingAreas(output.vehicle_footprints);
  output.processing_time_map["createVehiclePassingAreas"] = stop_watch.toc(true);

  const auto candidate_road_lanelets =
    getCandidateLanelets(input.route_lanelets, output.vehicle_footprints);
  const auto candidate_shoulder_lanelets =
    getCandidateLanelets(input.shoulder_lanelets, output.vehicle_footprints);
  output.candidate_lanelets = candidate_road_lanelets;
  output.candidate_lanelets.insert(
    output.candidate_lanelets.end(), candidate_shoulder_lanelets.begin(),
    candidate_shoulder_lanelets.end());

  output.processing_time_map["getCandidateLanelets"] = stop_watch.toc(true);

  output.will_leave_lane = willLeaveLane(output.candidate_lanelets, output.vehicle_footprints);
  output.processing_time_map["willLeaveLane"] = stop_watch.toc(true);

  output.is_out_of_lane = isOutOfLane(output.candidate_lanelets, output.vehicle_footprints.front());
  output.processing_time_map["isOutOfLane"] = stop_watch.toc(true);

  const double max_search_length_for_boundaries =
    calcMaxSearchLengthForBoundaries(*input.predicted_trajectory);
  const auto uncrossable_boundaries = extractUncrossableBoundaries(
    *input.lanelet_map, input.predicted_trajectory->points.front().pose.position,
    max_search_length_for_boundaries, input.boundary_types_to_detect);
  output.will_cross_boundary = willCrossBoundary(output.vehicle_footprints, uncrossable_boundaries);
  output.processing_time_map["willCrossBoundary"] = stop_watch.toc(true);

  return output;
}

bool LaneDepartureChecker::checkPathWillLeaveLane(
  const lanelet::ConstLanelets & lanelets, const PathWithLaneId & path) const
{
  std::vector<LinearRing2d> vehicle_footprints = createVehicleFootprints(path);
  lanelet::ConstLanelets candidate_lanelets = getCandidateLanelets(lanelets, vehicle_footprints);
  return willLeaveLane(candidate_lanelets, vehicle_footprints);
}

PoseDeviation LaneDepartureChecker::calcTrajectoryDeviation(
  const Trajectory & trajectory, const geometry_msgs::msg::Pose & pose, const double dist_threshold,
  const double yaw_threshold)
{
  const auto nearest_idx = motion_utils::findFirstNearestIndexWithSoftConstraints(
    trajectory.points, pose, dist_threshold, yaw_threshold);
  return tier4_autoware_utils::calcPoseDeviation(trajectory.points.at(nearest_idx).pose, pose);
}

TrajectoryPoints LaneDepartureChecker::resampleTrajectory(
  const Trajectory & trajectory, const double interval)
{
  TrajectoryPoints resampled;

  resampled.push_back(trajectory.points.front());
  for (size_t i = 1; i < trajectory.points.size() - 1; ++i) {
    const auto & point = trajectory.points.at(i);

    const auto p1 = tier4_autoware_utils::fromMsg(resampled.back().pose.position);
    const auto p2 = tier4_autoware_utils::fromMsg(point.pose.position);

    if (boost::geometry::distance(p1.to_2d(), p2.to_2d()) > interval) {
      resampled.push_back(point);
    }
  }
  resampled.push_back(trajectory.points.back());

  return resampled;
}

TrajectoryPoints LaneDepartureChecker::cutTrajectory(
  const TrajectoryPoints & trajectory, const double length)
{
  TrajectoryPoints cut;

  double total_length = 0.0;
  cut.push_back(trajectory.front());
  for (size_t i = 1; i < trajectory.size(); ++i) {
    const auto & point = trajectory.at(i);

    const auto p1 = tier4_autoware_utils::fromMsg(cut.back().pose.position);
    const auto p2 = tier4_autoware_utils::fromMsg(point.pose.position);
    const auto points_distance = boost::geometry::distance(p1.to_2d(), p2.to_2d());

    const auto remain_distance = length - total_length;

    // Over length
    if (remain_distance <= 0) {
      break;
    }

    // Require interpolation
    if (remain_distance <= points_distance) {
      const Eigen::Vector3d p_interpolated = p1 + remain_distance * (p2 - p1).normalized();

      TrajectoryPoint p;
      p.pose.position.x = p_interpolated.x();
      p.pose.position.y = p_interpolated.y();
      p.pose.position.z = p_interpolated.z();
      p.pose.orientation = point.pose.orientation;

      cut.push_back(p);
      break;
    }

    cut.push_back(point);
    total_length += points_distance;
  }

  return cut;
}

std::vector<LinearRing2d> LaneDepartureChecker::createVehicleFootprints(
  const geometry_msgs::msg::PoseWithCovariance & covariance, const TrajectoryPoints & trajectory,
  const Param & param)
{
  // Calculate longitudinal and lateral margin based on covariance
  const auto margin = calcFootprintMargin(covariance, param.footprint_margin_scale);

  // Create vehicle footprint in base_link coordinate
  const auto local_vehicle_footprint = vehicle_info_ptr_->createFootprint(margin.lat, margin.lon);

  // Create vehicle footprint on each TrajectoryPoint
  std::vector<LinearRing2d> vehicle_footprints;
  for (const auto & p : trajectory) {
    vehicle_footprints.push_back(
      transformVector(local_vehicle_footprint, tier4_autoware_utils::pose2transform(p.pose)));
  }

  return vehicle_footprints;
}

std::vector<LinearRing2d> LaneDepartureChecker::createVehicleFootprints(
  const PathWithLaneId & path) const
{
  // Create vehicle footprint in base_link coordinate
  const auto local_vehicle_footprint = vehicle_info_ptr_->createFootprint();

  // Create vehicle footprint on each Path point
  std::vector<LinearRing2d> vehicle_footprints;
  for (const auto & p : path.points) {
    vehicle_footprints.push_back(
      transformVector(local_vehicle_footprint, tier4_autoware_utils::pose2transform(p.point.pose)));
  }

  return vehicle_footprints;
}

std::vector<LinearRing2d> LaneDepartureChecker::createVehiclePassingAreas(
  const std::vector<LinearRing2d> & vehicle_footprints)
{
  // Create hull from two adjacent vehicle footprints
  std::vector<LinearRing2d> areas;
  for (size_t i = 0; i < vehicle_footprints.size() - 1; ++i) {
    const auto & footprint1 = vehicle_footprints.at(i);
    const auto & footprint2 = vehicle_footprints.at(i + 1);
    areas.push_back(createHullFromFootprints({footprint1, footprint2}));
  }

  return areas;
}

bool LaneDepartureChecker::willLeaveLane(
  const lanelet::ConstLanelets & candidate_lanelets,
  const std::vector<LinearRing2d> & vehicle_footprints)
{
  for (const auto & vehicle_footprint : vehicle_footprints) {
    if (isOutOfLane(candidate_lanelets, vehicle_footprint)) {
      return true;
    }
  }

  return false;
}

std::vector<std::pair<double, lanelet::Lanelet>> LaneDepartureChecker::getLaneletsFromPath(
  const lanelet::LaneletMapPtr lanelet_map_ptr, const PathWithLaneId & path) const
{
  // Get Footprint Hull basic polygon
  std::vector<LinearRing2d> vehicle_footprints = createVehicleFootprints(path);
  LinearRing2d footprint_hull = createHullFromFootprints(vehicle_footprints);
  auto to_basic_polygon = [](const LinearRing2d & footprint_hull) -> lanelet::BasicPolygon2d {
    lanelet::BasicPolygon2d basic_polygon;
    for (const auto & point : footprint_hull) {
      Eigen::Vector2d p(point.x(), point.y());
      basic_polygon.push_back(p);
    }
    return basic_polygon;
  };
  lanelet::BasicPolygon2d footprint_hull_basic_polygon = to_basic_polygon(footprint_hull);

  // Find all lanelets that intersect the footprint hull
  return lanelet::geometry::findWithin2d(
    lanelet_map_ptr->laneletLayer, footprint_hull_basic_polygon, 0.0);
}

std::optional<lanelet::BasicPolygon2d> LaneDepartureChecker::getFusedLaneletPolygonForPath(
  const lanelet::LaneletMapPtr lanelet_map_ptr, const PathWithLaneId & path) const
{
  const auto lanelets_distance_pair = getLaneletsFromPath(lanelet_map_ptr, path);
  // Fuse lanelets into a single BasicPolygon2d
  auto fused_lanelets = [&lanelets_distance_pair]() -> std::optional<lanelet::BasicPolygon2d> {
    if (lanelets_distance_pair.empty()) return std::nullopt;
    if (lanelets_distance_pair.size() == 1)
      return lanelets_distance_pair.at(0).second.polygon2d().basicPolygon();

    lanelet::BasicPolygon2d merged_polygon =
      lanelets_distance_pair.at(0).second.polygon2d().basicPolygon();
    for (size_t i = 1; i < lanelets_distance_pair.size(); ++i) {
      const auto & route_lanelet = lanelets_distance_pair.at(i).second;
      const auto & poly = route_lanelet.polygon2d().basicPolygon();

      std::vector<lanelet::BasicPolygon2d> lanelet_union_temp;
      boost::geometry::union_(poly, merged_polygon, lanelet_union_temp);

      // Update merged_polygon by accumulating all merged results
      merged_polygon.clear();
      for (const auto & temp_poly : lanelet_union_temp) {
        merged_polygon.insert(merged_polygon.end(), temp_poly.begin(), temp_poly.end());
      }
    }
    if (merged_polygon.empty()) return std::nullopt;
    return merged_polygon;
  }();

  return fused_lanelets;
}

bool LaneDepartureChecker::checkPathWillLeaveLane(
  const lanelet::LaneletMapPtr lanelet_map_ptr, const PathWithLaneId & path) const
{
  // check if the footprint is not fully contained within the fused lanelets polygon
  const std::vector<LinearRing2d> vehicle_footprints = createVehicleFootprints(path);
  const auto fused_lanelets_polygon = getFusedLaneletPolygonForPath(lanelet_map_ptr, path);
  if (!fused_lanelets_polygon) return true;
  return !std::all_of(
    vehicle_footprints.begin(), vehicle_footprints.end(),
    [&fused_lanelets_polygon](const auto & footprint) {
      return boost::geometry::within(footprint, fused_lanelets_polygon.value());
    });
}

PathWithLaneId LaneDepartureChecker::cropPointsOutsideOfLanes(
  const lanelet::LaneletMapPtr lanelet_map_ptr, const PathWithLaneId & path, const size_t end_index)
{
  PathWithLaneId temp_path;
  const auto fused_lanelets_polygon = getFusedLaneletPolygonForPath(lanelet_map_ptr, path);
  if (path.points.empty() || !fused_lanelets_polygon) return temp_path;
  const auto vehicle_footprints = createVehicleFootprints(path);
  size_t idx = 0;
  std::for_each(vehicle_footprints.begin(), vehicle_footprints.end(), [&](const auto & footprint) {
    if (idx > end_index || boost::geometry::within(footprint, fused_lanelets_polygon.value())) {
      temp_path.points.push_back(path.points.at(idx));
    }
    ++idx;
  });
  PathWithLaneId cropped_path = path;
  cropped_path.points = temp_path.points;
  return cropped_path;
}

bool LaneDepartureChecker::isOutOfLane(
  const lanelet::ConstLanelets & candidate_lanelets, const LinearRing2d & vehicle_footprint)
{
  for (const auto & point : vehicle_footprint) {
    if (!isInAnyLane(candidate_lanelets, point)) {
      return true;
    }
  }

  return false;
}

double LaneDepartureChecker::calcMaxSearchLengthForBoundaries(const Trajectory & trajectory) const
{
  const double max_ego_lon_length = std::max(
    std::abs(vehicle_info_ptr_->max_longitudinal_offset_m),
    std::abs(vehicle_info_ptr_->min_longitudinal_offset_m));
  const double max_ego_lat_length = std::max(
    std::abs(vehicle_info_ptr_->max_lateral_offset_m),
    std::abs(vehicle_info_ptr_->min_lateral_offset_m));
  const double max_ego_search_length = std::hypot(max_ego_lon_length, max_ego_lat_length);
  return calcArcLength(trajectory.points) + max_ego_search_length;
}

SegmentRtree LaneDepartureChecker::extractUncrossableBoundaries(
  const lanelet::LaneletMap & lanelet_map, const geometry_msgs::msg::Point & ego_point,
  const double max_search_length, const std::vector<std::string> & boundary_types_to_detect)
{
  const auto has_types =
    [](const lanelet::ConstLineString3d & ls, const std::vector<std::string> & types) {
      constexpr auto no_type = "";
      const auto type = ls.attributeOr(lanelet::AttributeName::Type, no_type);
      return (type != no_type && std::find(types.begin(), types.end(), type) != types.end());
    };

  SegmentRtree uncrossable_segments_in_range;
  LineString2d line;
  const auto ego_p = Point2d{ego_point.x, ego_point.y};
  for (const auto & ls : lanelet_map.lineStringLayer) {
    if (has_types(ls, boundary_types_to_detect)) {
      line.clear();
      for (const auto & p : ls) line.push_back(Point2d{p.x(), p.y()});
      for (auto segment_idx = 0LU; segment_idx + 1 < line.size(); ++segment_idx) {
        const Segment2d segment = {line[segment_idx], line[segment_idx + 1]};
        if (boost::geometry::distance(segment, ego_p) < max_search_length) {
          uncrossable_segments_in_range.insert(segment);
        }
      }
    }
  }
  return uncrossable_segments_in_range;
}

bool LaneDepartureChecker::willCrossBoundary(
  const std::vector<LinearRing2d> & vehicle_footprints, const SegmentRtree & uncrossable_segments)
{
  for (const auto & footprint : vehicle_footprints) {
    std::vector<Segment2d> intersection_result;
    uncrossable_segments.query(
      boost::geometry::index::intersects(footprint), std::back_inserter(intersection_result));
    if (!intersection_result.empty()) {
      return true;
    }
  }
  return false;
}

}  // namespace lane_departure_checker

// Copyright 2024 TIER IV, Inc.
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

#ifndef PERCEPTION_ONLINE_EVALUATOR__METRICS_CALCULATOR_HPP_
#define PERCEPTION_ONLINE_EVALUATOR__METRICS_CALCULATOR_HPP_

#include "perception_online_evaluator/metrics/deviation_metrics.hpp"
#include "perception_online_evaluator/metrics/metric.hpp"
#include "perception_online_evaluator/parameters.hpp"
#include "perception_online_evaluator/stat.hpp"

#include <rclcpp/time.hpp>

#include "autoware_auto_perception_msgs/msg/predicted_objects.hpp"
#include "geometry_msgs/msg/pose.hpp"
#include <unique_identifier_msgs/msg/uuid.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace perception_diagnostics
{
using autoware_auto_perception_msgs::msg::PredictedObject;
using autoware_auto_perception_msgs::msg::PredictedObjects;
using geometry_msgs::msg::Point;
using geometry_msgs::msg::Pose;
using unique_identifier_msgs::msg::UUID;

struct ObjectData
{
  PredictedObject object;
  std::vector<std::pair<Pose, Pose>> path_pairs;

  std::vector<Pose> getPredictedPath() const
  {
    std::vector<Pose> path;
    path.resize(path_pairs.size());
    std::transform(
      path_pairs.begin(), path_pairs.end(), path.begin(),
      [](const std::pair<Pose, Pose> & pair) -> Pose { return pair.first; });
    return path;
  }

  std::vector<Pose> getHistoryPath() const
  {
    std::vector<Pose> path;
    path.resize(path_pairs.size());
    std::transform(
      path_pairs.begin(), path_pairs.end(), path.begin(),
      [](const std::pair<Pose, Pose> & pair) -> Pose { return pair.second; });
    return path;
  }
};
using ObjectDataMap = std::unordered_map<std::string, ObjectData>;

// pair of history_path and smoothed_history_path for each object id
using HistoryPathMap =
  std::unordered_map<std::string, std::pair<std::vector<Pose>, std::vector<Pose>>>;

class MetricsCalculator
{
public:
  explicit MetricsCalculator(const std::shared_ptr<Parameters> & parameters)
  : parameters_(parameters){};

  /**
   * @brief calculate
   * @param [in] metric Metric enum value
   * @return map of string describing the requested metric and the calculated value
   */
  std::optional<MetricStatMap> calculate(const Metric & metric) const;

  /**
   * @brief set the dynamic objects used to calculate obstacle metrics
   * @param [in] objects predicted objects
   */
  void setPredictedObjects(const PredictedObjects & objects);

  HistoryPathMap getHistoryPathMap() const { return history_path_map_; }
  ObjectDataMap getDebugObjectData() const { return debug_target_object_; }

private:
  std::shared_ptr<Parameters> parameters_;

  // Store predicted objects information and calculation results
  std::unordered_map<std::string, std::map<rclcpp::Time, PredictedObject>> object_map_;
  HistoryPathMap history_path_map_;

  rclcpp::Time current_stamp_;

  // debug
  mutable ObjectDataMap debug_target_object_;

  // Functions to calculate history path
  void updateHistoryPath();
  std::vector<Pose> averageFilterPath(
    const std::vector<Pose> & path, const size_t window_size) const;
  std::vector<Pose> generateHistoryPathWithPrev(
    const std::vector<Pose> & prev_history_path, const Pose & new_pose, const size_t window_size);

  // Update object data
  void updateObjects(
    const std::string uuid, const rclcpp::Time stamp, const PredictedObject & object);
  void deleteOldObjects(const rclcpp::Time stamp);

  // Calculate metrics
  MetricStatMap calcLateralDeviationMetrics(const PredictedObjects & objects) const;
  MetricStatMap calcYawDeviationMetrics(const PredictedObjects & objects) const;
  MetricStatMap calcPredictedPathDeviationMetrics(const PredictedObjects & objects) const;
  Stat<double> calcPredictedPathDeviationMetrics(
    const PredictedObjects & objects, const double time_horizon) const;

  bool hasPassedTime(const rclcpp::Time stamp) const;
  bool hasPassedTime(const std::string uuid, const rclcpp::Time stamp) const;
  double getTimeDelay() const;

  // Extract object
  rclcpp::Time getClosestStamp(const rclcpp::Time stamp) const;
  std::optional<PredictedObject> getObjectByStamp(
    const std::string uuid, const rclcpp::Time stamp) const;
  PredictedObjects getObjectsByStamp(const rclcpp::Time stamp) const;

};  // class MetricsCalculator

}  // namespace perception_diagnostics

#endif  // PERCEPTION_ONLINE_EVALUATOR__METRICS_CALCULATOR_HPP_

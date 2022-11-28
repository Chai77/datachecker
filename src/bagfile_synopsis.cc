// MIT License

// Copyright (c) 2022 Joydeep Biswas

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//========================================================================
/*!
\file    bagfile_synopsis.cc
\brief   Generates a synopsis of a ROS bag file
\author  Joydeep Biswas, (C) 2012
*/
//========================================================================

#include <stdio.h>
#include <stdlib.h>

#include <math.h>
#include <string.h>
#include <vector>
#include <deque>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

#include "sensor_msgs/Image.h"
#include "sensor_msgs/LaserScan.h"
#include "rosbag/bag.h"
#include "rosbag/view.h"

using std::string;
using std::vector;
using std::deque;

struct TopicWindow {
  int windowSize = 0;
  std::deque<double> timestamps;
  double expectedRate;
  double expectedRateStdDev;
  double stdDevThreshold;
};

static std::unordered_map<std::string, TopicWindow> topicInfo;
static int total_obs = 0;
static int invalid_obs = 0;

void RegisterTopic(std::string topicName, double expectedRate, double expectedRateStdDev, int windowSize=10, double stdDevThreshold=10.0) {
  TopicWindow info;
  info.windowSize = windowSize;
  info.timestamps = std::deque<double>();
  info.expectedRate = expectedRate;
  info.expectedRateStdDev = expectedRateStdDev;
  info.stdDevThreshold = stdDevThreshold;

  topicInfo[topicName] = info;
}

void AddTopicObservation(std::string topicName, const rosbag::MessageInstance& message){
  total_obs+=1;

  int windowSize = topicInfo[topicName].windowSize;
  std::deque<double>& timestamps = topicInfo[topicName].timestamps;

  double expectedRate = topicInfo[topicName].expectedRate;
  double expectedRateStdDev = topicInfo[topicName].expectedRateStdDev;
  double stdDevThreshold = topicInfo[topicName].stdDevThreshold;

  double lowerRateBound = expectedRate - expectedRateStdDev * stdDevThreshold;
  double upperRateBound = expectedRate + expectedRateStdDev * stdDevThreshold;

  timestamps.push_back(message.getTime().toSec());

  if (timestamps.size() > (uint32_t)windowSize) {
    timestamps.pop_front();

    double avgRate = (windowSize-1) / (timestamps.back() - timestamps.front());

    if (avgRate < lowerRateBound || avgRate > upperRateBound){
      invalid_obs += 1;
      std::cout << "Rate " << avgRate << " " << topicInfo[topicName].expectedRate - 3 * topicInfo[topicName].expectedRateStdDev << " " << topicInfo[topicName].expectedRate + 3 * topicInfo[topicName].expectedRateStdDev << " " << total_obs << " " << invalid_obs << "\n";
    }
  }
}

bool GenerateBagfileSynopsis(const string& filename,
                             double* total_distance,
                             uint64_t* laser_scan_msgs_ptr,
                             uint64_t* kinect_scan_msgs_ptr,
                             uint64_t* kinect_image_msgs_ptr,
                             uint64_t* stargazer_sightings_ptr,
                             double* bag_duration_ptr,
                             double* enml_distance) {
  static const double kMinDistance = 30.0;
  static const bool debug = true;
  // printf("Reading %s\n", filename);
  rosbag::Bag bag;
  try {
    bag.open(filename,rosbag::bagmode::Read);
  } catch(rosbag::BagException exception) {
    printf("Unable to read %s, reason:\n %s\n", filename.c_str(), exception.what());
    return false;
  }
  
  std::vector<std::string> topics;
  std::vector<double> topic_mean_rate;
  std::vector<double> topic_std_dev_rate;
  
  //left: average: 9.977, std-dev: 0.0151
  //right: average: 10.002, std-dev: 0.00683
  topics.push_back("/stereo/left/image_raw/compressed");
  topics.push_back("/stereo/right/image_raw/compressed");

  topic_mean_rate.push_back(9.997);
  topic_mean_rate.push_back(10.082);

  topic_std_dev_rate.push_back(0.0151);
  topic_std_dev_rate.push_back(0.00683);

  rosbag::View view(bag, rosbag::TopicQuery(topics));

  double bag_time_start = -1.0;
  double bag_time = 0.0;
  double distance_traversed = 0.0;
  uint64_t& laser_scan_msgs = *laser_scan_msgs_ptr;
  uint64_t& kinect_scan_msgs = *kinect_scan_msgs_ptr;
  double& bag_duration = *bag_duration_ptr;
  laser_scan_msgs = 0;
  kinect_scan_msgs = 0;
  *total_distance = 0;
  bag_duration = 0.0;
  uint32_t autonomous_steps = 0;
  uint32_t total_steps = 0;
  bool autonomous = false;
  bool using_enml = true;

  for (uint32_t i = 0; i < topics.size(); i++){
    RegisterTopic(topics[i], topic_mean_rate[i], topic_std_dev_rate[i], 10);
  }

  for (rosbag::View::iterator it = view.begin(); it != view.end(); ++it) {
    const rosbag::MessageInstance& message = *it;
    bag_time = message.getTime().toSec();
    if (bag_time_start < 0.0) {
      // Initialize bag starting time.
      bag_time_start = bag_time;
    }

    std::string topic = message.getTopic();
    AddTopicObservation(topic, message);
  }
  bag_duration = bag_time - bag_time_start;
  const bool is_interesting =
      distance_traversed > kMinDistance &&
      (laser_scan_msgs > 0 || kinect_scan_msgs > 0);
  if (is_interesting) {
    *total_distance = *total_distance + distance_traversed;
    // Create a dummy file ".lta_interesting" to indicate that this bag file
    // is of interest for long-term autonomy (lta).
    // string interest_file = string(filename) + string(".lta_interesting");
    // ScopedFile fp(interest_file.c_str(), "w");
    // // Write a single byte, the newline character.
    // fprintf(fp, "\n");
  }
  if (debug) {
    printf("%s %9.1fs %7.1fm %6lu %6lu %9.1fm %6d %6d %d\n",
           filename.c_str(), bag_duration, distance_traversed,
           laser_scan_msgs, kinect_scan_msgs, *total_distance,
           autonomous_steps,
           total_steps, is_interesting?1:0);
  }
  string synopsis_file = string(filename) + string(".synopsis");

  FILE* fp = fopen(synopsis_file.c_str(), "w");
  // ScopedFile fp(synopsis_file.c_str(), "w");
  if (fp == NULL) {
    printf("Unable to write to %s\n", synopsis_file.c_str());
    return false;
  }
  fprintf(fp,
          "synopsis = {\n"
          "  filename = \"%s\";\n"
          "  duration = %9.1f;\n"
          "  autonomous_distance_traversed = %7.1f;\n"
          "  laser_msgs = %lu;\n"
          "  kinect_msgs = %lu;\n"
          "  kinect_images = %lu;\n"
          "  stargazer_sightings = %lu;\n"
          "  autonomous_steps = %d;\n"
          "  total_steps = %d;\n"
          "  using_enml = %s;\n"
          "};\n",
          filename.c_str(),
          bag_duration,
          distance_traversed,
          laser_scan_msgs,
          kinect_scan_msgs,
          (*kinect_image_msgs_ptr),
          (*stargazer_sightings_ptr),
          autonomous_steps,
          total_steps,
          using_enml ? "true" : "false");
  fclose(fp);
  if (!is_interesting) {
    bag_duration = 0.0;
    laser_scan_msgs = 0;
  }
  if (using_enml) {
    *enml_distance = distance_traversed;
  } else {
    *enml_distance = 0;
  }
  return is_interesting;
}

int main(int argc, char** argv) {

  
  //TODO: parse with YAML file. Currently experiencing some errors with import
  std::string bag_file = "/home/amrl-husky/Datasets/UTPeDa/110222/1667413740.bag";

  rosbag::Bag bag;
  try {
    bag.open(bag_file,rosbag::bagmode::Read);
  } catch(rosbag::BagException exception) {
    return -1;
  }

  rosbag::View view(bag); 

  int num_files = 1;
  vector<double> distances(num_files, 0.0);
  vector<double> enml_distances(num_files, 0.0);
  vector<double> durations(num_files, 0.0);
  vector<uint64_t> laser_scans(num_files, 0);
  vector<uint64_t> kinect_scans(num_files, 0);
  vector<uint64_t> kinect_images(num_files, 0);
  vector<uint64_t> stargazer_sigtings(num_files, 0);

  GenerateBagfileSynopsis(
        bag_file,
        &(distances[0]),
        &(laser_scans[0]),
        &(kinect_scans[0]),
        &(kinect_images[0]),
        &(stargazer_sigtings[0]),
        &(durations[0]),
        &(enml_distances[0]));

  return 0;
}

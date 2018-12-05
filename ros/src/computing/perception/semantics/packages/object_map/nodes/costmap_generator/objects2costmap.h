/*
 *  Copyright (c) 2018, Nagoya University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of Autoware nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ********************/
#ifndef OBJECTS2COSTMAP_H
#define OBJECTS2COSTMAP_H

// headers in ROS
#include <ros/ros.h>
#include <grid_map_ros/grid_map_ros.hpp>

// headers in local directory
#include "autoware_msgs/DetectedObjectArray.h"

class Objects2Costmap
{
  public:
    Objects2Costmap();
    ~Objects2Costmap();

    grid_map::Matrix makeCostmapFromObjects(const grid_map::GridMap& costmap,
                                             const std::string& gridmap_layer_name,
                                             const autoware_msgs::DetectedObjectArray::ConstPtr& in_objects);
  private:
    const int NUMBER_OF_POINTS = 4;
    const int NUMBER_OF_DIMENSIONS = 2;
    Eigen::MatrixXd makeRectanglePoints(const autoware_msgs::DetectedObject& in_object);
    grid_map::Polygon makePolygonFromObject(const autoware_msgs::DetectedObject& in_object);
    void setCostForPolygon(const grid_map::Polygon& polygon,const std::string& gridmap_layer_name,
                           grid_map::GridMap& objects_costmap);

};

#endif  // OBJECTS2COSTMAP_H
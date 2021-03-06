#include "perception/object_detector.h"
#include "perception/box_fitter.h"
#include "perception/typedefs.h"

#include "tf/transform_listener.h"

#include "pcl/PointIndices.h"
#include "pcl/point_cloud.h"
#include "pcl/point_types.h"
#include "pcl_conversions/pcl_conversions.h"
#include "pcl/filters/crop_box.h"
#include "pcl/filters/voxel_grid.h"
#include "pcl/common/angles.h"
#include "pcl/common/common.h"
#include "pcl/sample_consensus/method_types.h"
#include "pcl/sample_consensus/model_types.h"
#include "pcl/segmentation/sac_segmentation.h"
#include "pcl/segmentation/extract_clusters.h"
#include "pcl/filters/extract_indices.h"
#include "pcl_ros/transforms.h"
#include "shape_msgs/SolidPrimitive.h"

#include "geometry_msgs/Pose.h"
#include "geometry_msgs/Vector3.h"


#include "ros/ros.h"
#include "sensor_msgs/PointCloud2.h"
#include "visualization_msgs/Marker.h"


namespace perception {

ObjectDetector::ObjectDetector(const ros::Publisher& marker_pub): marker_pub_(marker_pub) {}

void ObjectDetector::downsampleCloud(pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud,
                    pcl::PointCloud<pcl::PointXYZRGB>::Ptr downsampled_cloud) {

  pcl::VoxelGrid<PointC> vox;
  vox.setInputCloud(cloud);
  double voxel_size;
  ros::param::param("voxel_size", voxel_size, 0.01);
  vox.setLeafSize(voxel_size, voxel_size, voxel_size);
  vox.filter(*downsampled_cloud);
  ROS_INFO("Downsampled to %ld points", downsampled_cloud->size());
}

bool ObjectDetector::cropCloud(pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud,
               pcl::PointCloud<pcl::PointXYZRGB>::Ptr cropped_cloud) {

  double min_x, min_y, min_z, max_x, max_y, max_z;
  ros::param::param("crop_min_x", min_x, 0.5);
  ros::param::param("crop_max_x", max_x, 1.5);
  ros::param::param("crop_min_y", min_y, -0.7);
  ros::param::param("crop_max_y", max_y, 0.7);
  ros::param::param("crop_min_z", min_z, 0.0);
  ros::param::param("crop_max_z", max_z, 0.5);
  Eigen::Vector4f min_pt(min_x, min_y, min_z, 1);
  Eigen::Vector4f max_pt(max_x, max_y, max_z, 1);

  pcl::CropBox<PointC> crop;
  crop.setInputCloud(cloud);
  crop.setMin(min_pt);
  crop.setMax(max_pt);
  crop.filter(*cropped_cloud);
  ROS_INFO("Cropped to %ld points", cropped_cloud->size());


  int min_cloud_size;
  ros::param::param("min_cloud_size", min_cloud_size, 100);

  if (cropped_cloud->size() > min_cloud_size) return 1;
  return 0; // rare case, but happens in simulation where the floor plane is perfectly z=0
}

void ObjectDetector::SegmentSurfaceObjects(pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud,
                           pcl::PointIndices::Ptr surface_indices,
                           std::vector<pcl::PointIndices>* object_indices) {

  // extract points cloud above the surface
  pcl::ExtractIndices<PointC> extract;
  pcl::PointIndices::Ptr above_surface_indices(new pcl::PointIndices());
  extract.setInputCloud(cloud);
  extract.setIndices(surface_indices);
  extract.setNegative(true);
  extract.filter(above_surface_indices->indices);

  ROS_INFO("There are %ld points above the surface", above_surface_indices->indices.size());

  // do euclidean clustering
  double cluster_tolerance;
  int min_cluster_size, max_cluster_size;
  ros::param::param("ec_cluster_tolerance", cluster_tolerance, 0.02);
  ros::param::param("ec_min_cluster_size", min_cluster_size, 40);
  ros::param::param("ec_max_cluster_size", max_cluster_size, 1500);

  pcl::EuclideanClusterExtraction<PointC> euclid;
  euclid.setInputCloud(cloud);
  euclid.setIndices(above_surface_indices);
  euclid.setClusterTolerance(cluster_tolerance);
  euclid.setMinClusterSize(min_cluster_size);
  euclid.setMaxClusterSize(max_cluster_size);
  euclid.extract(*object_indices);

  // Find the size of the smallest and the largest object,
  // where size = number of points in the cluster
  size_t min_size = std::numeric_limits<size_t>::max();
  size_t max_size = std::numeric_limits<size_t>::min();
  for (size_t i = 0; i < object_indices->size(); ++i) {
    size_t cluster_size = (*object_indices)[i].indices.size();
    min_size = cluster_size < min_size ? cluster_size : min_size;
    max_size = cluster_size > max_size ? cluster_size : max_size;
  }

  ROS_INFO("Found %ld objects, min size: %ld, max size: %ld", object_indices->size(), min_size, max_size);
}

void ObjectDetector::SegmentSurface(PointCloudC::Ptr cloud, pcl::PointIndices::Ptr indices, pcl::ModelCoefficients::Ptr coeff) {
  pcl::PointIndices indices_internal;
  pcl::SACSegmentation<PointC> seg;

  seg.setOptimizeCoefficients(true);
  // Search for a plane perpendicular to some axis (specified below).
  seg.setModelType(pcl::SACMODEL_PERPENDICULAR_PLANE);
  seg.setMethodType(pcl::SAC_RANSAC);
  // Set the distance to the plane for a point to be an inlier.
  seg.setDistanceThreshold(0.01);
  seg.setInputCloud(cloud);

  // Make sure that the plane is perpendicular to Z-axis, 10 degree tolerance.
  Eigen::Vector3f axis;
  axis << 0, 0, 1;
  seg.setAxis(axis);
  seg.setEpsAngle(pcl::deg2rad(10.0));

  // coeff contains the coefficients of the plane:
  // ax + by + cz + d = 0
  seg.segment(indices_internal, *coeff);

  // *indices = indices_internal;

  // Build custom indices that ignores points above the plane.
  double distance_above_plane;
  ros::param::param("distance_above_plane", distance_above_plane, 0.02);

  for (size_t i = 0; i < cloud->size(); ++i) {
    const PointC& pt = cloud->points[i];
    float val = coeff->values[0] * pt.x + coeff->values[1] * pt.y +
                coeff->values[2] * pt.z + coeff->values[3];
    if (val <= distance_above_plane) {
      indices->indices.push_back(i);
    }
  }

  if (indices->indices.size() == 0) {
    ROS_ERROR("Unable to find surface.");
    return;
  }
}

bool ObjectDetector::checkShapeAndPose(shape_msgs::SolidPrimitive shape, geometry_msgs::Pose pose) {
  double object_max_dim, object_max_grab_dim, object_min_height, object_pose_min_height;
  ros::param::param("object_max_dim", object_max_dim, 0.3);
  ros::param::param("object_max_grab_dim", object_max_grab_dim, 0.1);
  ros::param::param("object_min_height", object_min_height, 0.02);
  ros::param::param("object_pose_min_height", object_pose_min_height, 0.03);

  double min_xy_dim = (shape.dimensions[0] < shape.dimensions[1]) ? shape.dimensions[0] : shape.dimensions[1];

  return shape.dimensions[0] < object_max_dim &&
         shape.dimensions[1] < object_max_dim &&
         shape.dimensions[2] < object_max_dim &&
         min_xy_dim < object_max_grab_dim &&
         shape.dimensions[2] > object_min_height &&
         pose.position.z > object_pose_min_height;
}

void ObjectDetector::visualizeNewObjects(shape_msgs::SolidPrimitive shape, geometry_msgs::Pose obj_pose, size_t id,
                                          visualization_msgs::Marker& object_marker, visualization_msgs::Marker& orient_marker) {

  // publish an arrow marker that shows object orientation
  orient_marker.ns = "orientations";
  orient_marker.id = id;
  orient_marker.header.frame_id = "base_link";
  orient_marker.type = visualization_msgs::Marker::ARROW;
  orient_marker.action = visualization_msgs::Marker::ADD;
  orient_marker.pose = obj_pose;

  orient_marker.scale.x = 0.2;
  orient_marker.scale.y = 0.01;
  orient_marker.scale.z = 0.01;
  orient_marker.color.r = 1;
  orient_marker.color.a = 1;
  marker_pub_.publish(orient_marker);

  // Publish a bounding box marker around the object
  object_marker.ns = "objects";
  object_marker.id = id;
  object_marker.header.frame_id = "base_link";
  object_marker.type = visualization_msgs::Marker::CUBE;
  object_marker.action = visualization_msgs::Marker::ADD;
  object_marker.pose = obj_pose;

  object_marker.scale.x = shape.dimensions[0];
  object_marker.scale.y = shape.dimensions[1];
  object_marker.scale.z = shape.dimensions[2];

  object_marker.color.g = 1;
  object_marker.color.a = 1;

  marker_pub_.publish(object_marker);
}

void ObjectDetector::deleteOldObjects(std::vector<visualization_msgs::Marker>& objects) {
  for (size_t i = 0; i < objects.size(); ++i) {
      visualization_msgs::Marker object_marker = objects[i];
      object_marker.action = visualization_msgs::Marker::DELETE;
      marker_pub_.publish(object_marker);
  }
}

void ObjectDetector::Callback(const sensor_msgs::PointCloud2& msg) {

  // Transform the cloud msg into base_link
  tf_listener.waitForTransform("base_link", msg.header.frame_id, ros::Time(0), ros::Duration(5.0));

  tf::StampedTransform transform;
  try {
    tf_listener.lookupTransform("base_link", msg.header.frame_id, ros::Time(0), transform);
  } catch (tf::LookupException& e) {
    std::cerr << e.what() << std::endl;
    return;
  } catch (tf::ExtrapolationException& e) {
    std::cerr << e.what() << std::endl;
    return;
  }

  sensor_msgs::PointCloud2 msg_base;
  pcl_ros::transformPointCloud("base_link", transform, msg, msg_base);

  // convert ros cloud to pcl cloud
  PointCloudC::Ptr cloud(new PointCloudC());
  pcl::fromROSMsg(msg_base, *cloud);
  ROS_INFO("Got point cloud with %ld points", cloud->size());

  PointCloudC::Ptr cropped_cloud(new PointCloudC());
  PointCloudC::Ptr downsampled_cloud(new PointCloudC());

  bool has_data = cropCloud(cloud, cropped_cloud);
  if (!has_data) {
    ROS_INFO("Cropped cloud doesn't have enough points, detection skipped...");
    return;
  }
  downsampleCloud(cropped_cloud, downsampled_cloud);

  pcl::PointIndices::Ptr surface_inliers(new pcl::PointIndices());
  pcl::ModelCoefficients::Ptr coeff(new pcl::ModelCoefficients());
  SegmentSurface(downsampled_cloud, surface_inliers, coeff);

  // get objects cloud indices
  std::vector<pcl::PointIndices> object_indices;
  SegmentSurfaceObjects(downsampled_cloud, surface_inliers, &object_indices);

  // extract object points from downsampled_cloud
  pcl::ExtractIndices<PointC> extract;
  extract.setInputCloud(downsampled_cloud);

  // delete old objects
  deleteOldObjects(prev_objects);
  prev_objects.clear();

  // visualize new objects!
  std::vector<visualization_msgs::Marker> cur_objects;

  for (size_t i = 0; i < object_indices.size(); ++i) {
    // Reify indices into a point cloud of the object.
    pcl::PointIndices::Ptr indices(new pcl::PointIndices);
    *indices = object_indices[i];
    PointCloudC::Ptr object_cloud(new PointCloudC());
    extract.setIndices(indices);
    extract.setNegative(false);
    extract.filter(*object_cloud);

    // try to fit a bounding box to the object
    PointCloudC::Ptr extract_out(new PointCloudC());
    shape_msgs::SolidPrimitive shape;
    geometry_msgs::Pose obj_pose;
    FitBox(*object_cloud, coeff, *extract_out, shape, obj_pose);

    // filter objects with dimensions that are too large to grasp
    if (!checkShapeAndPose(shape, obj_pose)) continue;

    visualization_msgs::Marker object_marker, orient_marker;
    visualizeNewObjects(shape, obj_pose, i, object_marker, orient_marker);

    cur_objects.push_back(object_marker);
    cur_objects.push_back(orient_marker);
  }

  prev_objects = cur_objects;
}

}  // namespace perception

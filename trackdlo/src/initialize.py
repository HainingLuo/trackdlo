#!/usr/bin/env python3

import rospy
import numpy as np
np.float = np.float64
import ros_numpy
from sensor_msgs.msg import PointCloud2, PointField, Image, CameraInfo
import sensor_msgs.point_cloud2 as pcl2
import std_msgs.msg
import message_filters

import struct
import time
import cv2
import time

from visualization_msgs.msg import MarkerArray
from scipy import interpolate

from utils import extract_connected_skeleton, ndarray2MarkerArray

proj_matrix = None
def camera_info_callback (info):
    global proj_matrix
    proj_matrix = np.array(list(info.P)).reshape(3, 4)
    print('Received camera projection matrix:')
    print(proj_matrix)
    camera_info_sub.unregister()

def color_thresholding (hsv_image, cur_depth):
    global lower, upper

    mask_dlo = cv2.inRange(hsv_image.copy(), lower, upper).astype('uint8')

    # tape green
    lower_green = (58, 130, 50)
    upper_green = (90, 255, 89)
    mask_green = cv2.inRange(hsv_image.copy(), lower_green, upper_green).astype('uint8')

    # combine masks
    mask = cv2.bitwise_or(mask_green.copy(), mask_dlo.copy())

    # filter mask base on depth values
    mask[cur_depth < 0.57*1000] = 0

    return mask, mask_green

def remove_duplicate_rows(array):
    _, idx = np.unique(array, axis=0, return_index=True)
    data = array[np.sort(idx)]
    rospy.set_param('/init_tracker/num_of_nodes', len(data))
    return data

def callback (rgb, depth):
    global lower, upper

    print("Initializing...")

    # process rgb image
    cur_image = ros_numpy.numpify(rgb)
    hsv_image = cv2.cvtColor(cur_image.copy(), cv2.COLOR_RGB2HSV)

    # process depth image
    cur_depth = ros_numpy.numpify(depth)

    if not multi_color_dlo:
        # color thresholding
        mask = cv2.inRange(hsv_image, lower, upper)
    else:
        # color thresholding
        mask, mask_tip = color_thresholding(hsv_image, cur_depth)
    try:
        start_time = time.time()
        mask = cv2.cvtColor(mask.copy(), cv2.COLOR_GRAY2BGR)

        # returns the pixel coord of points (in order). a list of lists
        img_scale = 1
        extracted_chains = extract_connected_skeleton(visualize_initialization_process, mask, img_scale=img_scale, seg_length=8, max_curvature=25)

        all_pixel_coords = []
        for chain in extracted_chains:
            all_pixel_coords += chain
        print('Finished extracting chains. Time taken:', time.time()-start_time)

        all_pixel_coords = np.array(all_pixel_coords) * img_scale
        all_pixel_coords = np.flip(all_pixel_coords, 1)

        pc_z = cur_depth[tuple(map(tuple, all_pixel_coords.T))] / 1000.0
        fx = proj_matrix[0, 0]
        fy = proj_matrix[1, 1]
        cx = proj_matrix[0, 2]
        cy = proj_matrix[1, 2]
        pixel_x = all_pixel_coords[:, 1]
        pixel_y = all_pixel_coords[:, 0]
        # if the first mask value is not in the tip mask, reverse the pixel order
        if multi_color_dlo:
            pixel_value1 = mask_tip[pixel_y[-1],pixel_x[-1]]
            if pixel_value1 == 255:
                pixel_x, pixel_y = pixel_x[::-1], pixel_y[::-1]

        pc_x = (pixel_x - cx) * pc_z / fx
        pc_y = (pixel_y - cy) * pc_z / fy
        extracted_chains_3d = np.vstack((pc_x, pc_y))
        extracted_chains_3d = np.vstack((extracted_chains_3d, pc_z))
        extracted_chains_3d = extracted_chains_3d.T

        # do not include those without depth values
        extracted_chains_3d = extracted_chains_3d[((extracted_chains_3d[:, 0] != 0) | (extracted_chains_3d[:, 1] != 0) | (extracted_chains_3d[:, 2] != 0))]

        if multi_color_dlo:
            depth_threshold = 0.57  # m
            extracted_chains_3d = extracted_chains_3d[extracted_chains_3d[:, 2] > depth_threshold]

        # tck, u = interpolate.splprep(extracted_chains_3d.T, s=0.001)
        tck, u = interpolate.splprep(extracted_chains_3d.T, s=0.0005)
        # 1st fit, less points
        u_fine = np.linspace(0, 1, 300) # <-- num fit points
        x_fine, y_fine, z_fine = interpolate.splev(u_fine, tck)
        spline_pts = np.vstack((x_fine, y_fine, z_fine)).T

        # 2nd fit, higher accuracy
        num_true_pts = int(np.sum(np.sqrt(np.sum(np.square(np.diff(spline_pts, axis=0)), axis=1))) * 1000)
        u_fine = np.linspace(0, 1, num_true_pts) # <-- num true points
        x_fine, y_fine, z_fine = interpolate.splev(u_fine, tck)
        spline_pts = np.vstack((x_fine, y_fine, z_fine)).T

        nodes = spline_pts[np.linspace(0, num_true_pts-1, num_of_nodes).astype(int)]

        init_nodes = remove_duplicate_rows(nodes)
        # results = ndarray2MarkerArray(init_nodes, result_frame_id, [1, 150/255, 0, 0.75], [0, 1, 0, 0.75])
        results = ndarray2MarkerArray(init_nodes, result_frame_id, [0, 149/255, 203/255, 0.75], [0, 149/255, 203/255, 0.75])
        results_pub.publish(results)

        # add color
        pc_rgba = struct.unpack('I', struct.pack('BBBB', 255, 40, 40, 255))[0]
        pc_rgba_arr = np.full((len(init_nodes), 1), pc_rgba)
        pc_colored = np.hstack((init_nodes, pc_rgba_arr)).astype(object)
        pc_colored[:, 3] = pc_colored[:, 3].astype(int)

        header.stamp = rospy.Time.now()
        converted_points = pcl2.create_cloud(header, fields, pc_colored)
        pc_pub.publish(converted_points)
    except:
        rospy.logerr("Failed to extract splines.")
        rospy.signal_shutdown('Stopping initialization.')

if __name__=='__main__':
    rospy.init_node('init_tracker', anonymous=True)

    global new_messages, lower, upper
    new_messages=False

    num_of_nodes = rospy.get_param('/init_tracker/num_of_nodes')
    multi_color_dlo = rospy.get_param('/init_tracker/multi_color_dlo')
    camera_info_topic = rospy.get_param('/init_tracker/camera_info_topic')
    rgb_topic = rospy.get_param('/init_tracker/rgb_topic')
    depth_topic = rospy.get_param('/init_tracker/depth_topic')
    result_frame_id = rospy.get_param('/init_tracker/result_frame_id')
    visualize_initialization_process = rospy.get_param('/init_tracker/visualize_initialization_process')

    hsv_threshold_upper_limit = rospy.get_param('/init_tracker/hsv_threshold_upper_limit')
    hsv_threshold_lower_limit = rospy.get_param('/init_tracker/hsv_threshold_lower_limit')

    upper_array = hsv_threshold_upper_limit.split(' ')
    lower_array = hsv_threshold_lower_limit.split(' ')
    upper = (int(upper_array[0]), int(upper_array[1]), int(upper_array[2]))
    lower = (int(lower_array[0]), int(lower_array[1]), int(lower_array[2]))

    camera_info_sub = rospy.Subscriber(camera_info_topic, CameraInfo, camera_info_callback)
    rgb_sub = message_filters.Subscriber(rgb_topic, Image)
    depth_sub = message_filters.Subscriber(depth_topic, Image)

    # header
    header = std_msgs.msg.Header()
    header.stamp = rospy.Time.now()
    header.frame_id = result_frame_id
    fields = [PointField('x', 0, PointField.FLOAT32, 1),
                PointField('y', 4, PointField.FLOAT32, 1),
                PointField('z', 8, PointField.FLOAT32, 1),
                PointField('rgba', 12, PointField.UINT32, 1)]
    pc_pub = rospy.Publisher ('/trackdlo/init_nodes', PointCloud2, queue_size=10)
    results_pub = rospy.Publisher ('/trackdlo/init_nodes_markers', MarkerArray, queue_size=10)

    ts = message_filters.TimeSynchronizer([rgb_sub, depth_sub], 10)
    ts.registerCallback(callback)

    rospy.spin()
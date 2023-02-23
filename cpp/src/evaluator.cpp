#include "../include/utils.h"
#include "../include/trackdlo.h"
#include "../include/evaluator.h"

using Eigen::MatrixXd;
using Eigen::MatrixXf;
using Eigen::RowVectorXf;
using Eigen::RowVectorXd;
using cv::Mat;

int bag_file = 2;

evaluator::evaluator () {}
evaluator::evaluator (int length, int trial, double pct_occlusion, std::string alg) {
    length_ = length;
    trial_ = trial;
    pct_occlusion_ = pct_occlusion;
    alg_ = alg;
}

MatrixXf evaluator::sort_pts (MatrixXf Y_0, MatrixXf head) {
    int N = Y_0.rows();
    MatrixXf Y_0_sorted = MatrixXf::Zero(N, 3);
    std::vector<MatrixXf> Y_0_sorted_vec = {};
    std::vector<bool> selected_node(N, false);
    selected_node[0] = true;
    int last_visited_b = 0;

    MatrixXf G = MatrixXf::Zero(N, N);
    for (int i = 0; i < N; i ++) {
        for (int j = 0; j < N; j ++) {
            G(i, j) = (Y_0.row(i) - Y_0.row(j)).squaredNorm();
        }
    }

    int reverse = 0;
    int counter = 0;
    int reverse_on = 0;
    int insertion_counter = 0;

    while (counter < N-1) {
        double minimum = INFINITY;
        int a = 0;
        int b = 0;

        for (int m = 0; m < N; m ++) {
            if (selected_node[m] == true) {
                for (int n = 0; n < N; n ++) {
                    if ((!selected_node[n]) && (G(m, n) != 0.0)) {
                        if (minimum > G(m, n)) {
                            minimum = G(m, n);
                            a = m;
                            b = n;
                        }
                    }
                }
            }
        }

        if (counter == 0) {
            Y_0_sorted_vec.push_back(Y_0.row(a));
            Y_0_sorted_vec.push_back(Y_0.row(b));
        }
        else {
            if (last_visited_b != a) {
                reverse += 1;
                reverse_on = a;
                insertion_counter = 1;
            }
            
            if (reverse % 2 == 1) {
                auto it = find(Y_0_sorted_vec.begin(), Y_0_sorted_vec.end(), Y_0.row(a));
                Y_0_sorted_vec.insert(it, Y_0.row(b));
            }
            else if (reverse != 0) {
                auto it = find(Y_0_sorted_vec.begin(), Y_0_sorted_vec.end(), Y_0.row(reverse_on));
                Y_0_sorted_vec.insert(it + insertion_counter, Y_0.row(b));
                insertion_counter += 1;
            }
            else {
                Y_0_sorted_vec.push_back(Y_0.row(b));
            }
        }

        last_visited_b = b;
        selected_node[b] = true;
        counter += 1;
    }

    if (pt2pt_dis(Y_0_sorted_vec[0], head) > 0.05) {
        std::reverse(Y_0_sorted_vec.begin(), Y_0_sorted_vec.end());
    }

    // copy to Y_0_sorted
    for (int i = 0; i < N; i ++) {
        Y_0_sorted.row(i) = Y_0_sorted_vec[i];
    }

    return Y_0_sorted;
}

MatrixXf evaluator::get_ground_truth_nodes (Mat rgb_img, pcl::PointCloud<pcl::PointXYZRGB> cloud_xyz) {
    Mat mask_blue, mask_red_1, mask_red_2, mask_red, mask_yellow, mask_markers, mask, mask_rgb;
    Mat cur_image_hsv, mask_without_occlusion_block;

    // convert color
    cv::cvtColor(rgb_img, cur_image_hsv, cv::COLOR_BGR2HSV);

    std::vector<int> lower_blue = {90, 80, 80};
    std::vector<int> upper_blue = {130, 255, 255};

    std::vector<int> lower_red_1 = {130, 60, 50};
    std::vector<int> upper_red_1 = {255, 255, 255};

    std::vector<int> lower_red_2 = {0, 60, 50};
    std::vector<int> upper_red_2 = {10, 255, 255};

    std::vector<int> lower_yellow = {15, 100, 80};
    std::vector<int> upper_yellow = {40, 255, 255};

    // filter blue
    cv::inRange(cur_image_hsv, cv::Scalar(lower_blue[0], lower_blue[1], lower_blue[2]), cv::Scalar(upper_blue[0], upper_blue[1], upper_blue[2]), mask_blue);

    // filter red
    cv::inRange(cur_image_hsv, cv::Scalar(lower_red_1[0], lower_red_1[1], lower_red_1[2]), cv::Scalar(upper_red_1[0], upper_red_1[1], upper_red_1[2]), mask_red_1);
    cv::inRange(cur_image_hsv, cv::Scalar(lower_red_2[0], lower_red_2[1], lower_red_2[2]), cv::Scalar(upper_red_2[0], upper_red_2[1], upper_red_2[2]), mask_red_2);

    // filter yellow
    cv::inRange(cur_image_hsv, cv::Scalar(lower_yellow[0], lower_yellow[1], lower_yellow[2]), cv::Scalar(upper_yellow[0], upper_yellow[1], upper_yellow[2]), mask_yellow);

    // combine red mask
    cv::bitwise_or(mask_red_1, mask_red_2, mask_red);
    // combine overall mask
    cv::bitwise_or(mask_red, mask_blue, mask_without_occlusion_block);
    cv::bitwise_or(mask_yellow, mask_without_occlusion_block, mask_without_occlusion_block);
    cv::bitwise_or(mask_red, mask_yellow, mask_markers);

    // simple blob detector
    std::vector<cv::KeyPoint> keypoints_markers;
    // std::vector<cv::KeyPoint> keypoints_blue;
    cv::SimpleBlobDetector::Params blob_params;
    blob_params.filterByColor = false;
    blob_params.filterByArea = true;
    blob_params.minArea = 10;
    blob_params.filterByCircularity = false;
    blob_params.filterByInertia = true;
    blob_params.filterByConvexity = false;
    cv::Ptr<cv::SimpleBlobDetector> detector = cv::SimpleBlobDetector::create(blob_params);
    // detect
    detector->detect(mask_markers, keypoints_markers);
    // detector->detect(mask_blue, keypoints_blue);

    pcl::PointCloud<pcl::PointXYZRGB> cur_nodes_xyz;

    for (cv::KeyPoint key_point : keypoints_markers) {
        auto keypoint_pc = cloud_xyz(static_cast<int>(key_point.pt.x), static_cast<int>(key_point.pt.y));
        
        if (bag_file == 2) {
            if (keypoint_pc.x < -0.15 || keypoint_pc.y < -0.15 || keypoint_pc.z < 0.58) {
                continue;
            }
        }

        if (keypoint_pc.z > 0.58) {
            cur_nodes_xyz.push_back(keypoint_pc);
        }
    }

    // the node set returned is not sorted
    return cur_nodes_xyz.getMatrixXfMap().topRows(3).transpose();
}

double evaluator::calc_min_distance (MatrixXf A, MatrixXf B, MatrixXf E, MatrixXf& closest_pt_on_AB_to_E) {
    MatrixXf AB = B - A;
    MatrixXf AE = E - A;

    double distance = cross_product(AE, AB).norm() / AB.norm();
    closest_pt_on_AB_to_E = A + AB*dot_product(AE, AB) / dot_product(AB, AB);

    MatrixXf AP = closest_pt_on_AB_to_E - A;
    if (dot_product(AP, AB) < 0 || dot_product(AP, AB) > dot_product(AB, AB)) {
        MatrixXf BE = E - B;
        double distance_AE = sqrt(dot_product(AE, AE));
        double distance_BE = sqrt(dot_product(BE, BE));
        if (distance_AE > distance_BE) {
            distance = distance_BE;
            closest_pt_on_AB_to_E = B.replicate(1, 1);
        }
        else {
            distance = distance_AE;
            closest_pt_on_AB_to_E = A.replicate(1, 1);
        }
    }

    return distance;
}

double evaluator::get_piecewise_error (MatrixXf Y_track, MatrixXf Y_true) {
    double total_distances_to_curve = 0.0;
    std::vector<MatrixXf> closest_pts_on_Y_true = {};

    for (int idx = 0; idx < Y_track.rows(); idx ++) {
        double dist = -1;
        MatrixXf closest_pt = MatrixXf::Zero(1, 3);

        for (int i = 0; i < Y_true.rows()-1; i ++) {
            MatrixXf closest_pt_i = MatrixXf::Zero(1, 3);
            double dist_i = calc_min_distance(Y_true.row(i), Y_true.row(i+1), Y_track.row(idx), closest_pt_i);
            if (dist == -1 || dist_i < dist) {
                dist = dist_i;
                closest_pt = closest_pt_i.replicate(1, 1);
            }
        }

        total_distances_to_curve += dist;
        closest_pts_on_Y_true.push_back(closest_pt);
    }

    double error_frame = total_distances_to_curve / Y_track.rows();

    return error_frame;
}
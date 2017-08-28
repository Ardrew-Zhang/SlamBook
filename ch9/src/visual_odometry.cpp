//
// Created by Left Thomas on 2017/8/27.
// bundle adjustment不能做，在Mac上会报
// malloc: *** mach_vm_map(size=9288052384388087808) failed (error code=3)
// *** error: can't allocate region
// 错误
// 这里删除了，可以上GitHub看历史commit
//

#include "myslam/visual_odometry.h"
#include "myslam/config.h"
#include <opencv2/calib3d/calib3d.hpp>

namespace myslam {

    VisualOdometry::VisualOdometry() : state_(INITIALIZING), map_(new Map), ref_(nullptr), curr_(nullptr),
                                       num_inliers_(0), num_lost_(0),
                                       matcher_flann_(new cv::flann::LshIndexParams(5, 10, 2)) {
        num_of_features_ = Config::get<int>("number_of_features");
        scale_factor_ = Config::get<float>("scale_factor");
        level_pyramid_ = Config::get<int>("level_pyramid");
        match_ratio_ = Config::get<float>("match_ratio");
        max_num_lost_ = Config::get<int>("max_num_lost");
        min_inliers_ = Config::get<int>("min_inliers");
        key_frame_min_rot_ = Config::get<double>("keyframe_rotation");
        key_frame_min_trans_ = Config::get<double>("keyframe_translation");
        map_point_erase_ratio_ = Config::get<double>("map_point_erase_ratio");
        orb_ = cv::ORB::create(num_of_features_, scale_factor_, level_pyramid_);
    }

    VisualOdometry::~VisualOdometry() = default;

    bool VisualOdometry::addFrame(Frame::Ptr frame) {
        switch (state_) {
            case INITIALIZING: {
                state_ = OK;
                curr_ = ref_ = frame;
//                extract features from first frame
                extractKeyPoints();
                computeDescriptors();
                addKeyFrame();
                break;
            }
            case OK: {
                curr_ = frame;
                curr_->T_c_w_ = ref_->T_c_w_;
                extractKeyPoints();
                computeDescriptors();
                featuresMatching();
                poseEstimationPnP();
                if (checkEstimatedPose()) {
                    curr_->T_c_w_ = T_c_r_estimated_;
                    optimizeMap();
                    num_lost_ = 0;
                    if (checkKeyFrame())
                        addKeyFrame();
                } else {
                    num_lost_++;
                    if (num_lost_ > max_num_lost_)
                        state_ = LOST;
                    return false;
                }
                break;
            }
            case LOST: {
                cout << "vo has lost." << endl;
                break;
            }
        }
        return true;
    }

    void VisualOdometry::extractKeyPoints() {
        orb_->detect(curr_->color_, keypoints_curr_);
    }

    void VisualOdometry::computeDescriptors() {
        orb_->compute(curr_->color_, keypoints_curr_, descriptors_curr_);
    }

    void VisualOdometry::featuresMatching() {
        vector<cv::DMatch> matches;
//        select the candidates in map
        Mat desp_map;
        vector<MapPoint::Ptr> candidates;
        for (auto &allpoints:map_->map_points_) {
            MapPoint::Ptr &p = allpoints.second;
//            check if p in curr frame image
            if (curr_->isInFrame(p->pos_)) {
//                add to candidate
                p->visible_times_++;
                candidates.push_back(p);
                desp_map.push_back(p->descriptor_);
            }
        }
        matcher_flann_.match(desp_map, descriptors_curr_, matches, cv::noArray());

        float min_dist = min_element(matches.begin(), matches.end(), [](
                const cv::DMatch &m1, const cv::DMatch &m2) {
            return m1.distance < m2.distance;
        })->distance;

        match_3dpts_.clear();
        match_2dkp_index_.clear();
        for (cv::DMatch &m:matches) {
            if (m.distance < max<float>(match_ratio_ * min_dist, 30.0)) {
                match_3dpts_.push_back(candidates[m.queryIdx]);
                match_2dkp_index_.push_back(m.trainIdx);
            }
        }
        cout << "good matches:" << match_3dpts_.size() << endl;
    }

    void VisualOdometry::poseEstimationPnP() {
        vector<cv::Point3f> pts_3d;
        vector<cv::Point2f> pts_2d;
        for (int index :match_2dkp_index_) {
            pts_2d.push_back(keypoints_curr_[index].pt);
        }
        for (const MapPoint::Ptr &pt :match_3dpts_) {
            pts_3d.push_back(pt->getPositionCV());
        }

        cv::Mat_<double> K(3, 3);
        K << ref_->camera_->fx_, 0, ref_->camera_->cx_, 0, ref_->camera_->fy_, ref_->camera_->cy_, 0, 0, 1;
        Mat rvec, tvec, inliers;
        cv::solvePnPRansac(pts_3d, pts_2d, K, Mat(), rvec, tvec, false, 100, 4.0, 0.99, inliers);
        num_inliers_ = inliers.rows;
//        cout<<"PnP inliers: "<<num_inliers_<<endl;
        T_c_r_estimated_ = SE3(SO3(rvec.at<double>(0, 0), rvec.at<double>(1, 0), rvec.at<double>(2, 0)),
                               Vector3d(tvec.at<double>(0, 0), tvec.at<double>(1, 0), tvec.at<double>(2, 0)));
    }

    bool VisualOdometry::checkEstimatedPose() {
        if (num_inliers_ < min_inliers_) {
            cout << "reject because the number of inliers is too small: " << num_inliers_ << endl;
            return false;
        }
        SE3 T_r_c = ref_->T_c_w_ * T_c_r_estimated_.inverse();
        Sophus::Vector6d d = T_r_c.log();
        if (d.norm() > 5.0) {
            cout << "reject because the motion is too large: " << d.norm() << endl;
            return false;
        }
        return true;
    }

    bool VisualOdometry::checkKeyFrame() {
        SE3 T_r_c = ref_->T_c_w_ * T_c_r_estimated_.inverse();
        Sophus::Vector6d d = T_r_c.log();
//        注意，平移在前，旋转在后
        Vector3d trans = d.head(3);
        Vector3d rot = d.tail(3);
        return trans.norm() > key_frame_min_trans_ || rot.norm() > key_frame_min_rot_;
    }

    void VisualOdometry::addKeyFrame() {
//        cout << "adding a key frame " << endl;
        if (map_->key_frames_.empty()) {
//            first key frame, add all 3d points into map
            for (int i = 0; i < keypoints_curr_.size(); ++i) {
                double d = curr_->findDepth(keypoints_curr_[i]);
                if (d < 0)
                    continue;
                Vector3d p_world = ref_->camera_->pixel2world(Vector2d(
                        keypoints_curr_[i].pt.x, keypoints_curr_[i].pt.y), curr_->T_c_w_, d);
                Vector3d n = p_world - ref_->getCameraCenter();
                n.normalize();
                MapPoint::Ptr map_point = MapPoint::createMapPoint(
                        p_world, n, descriptors_curr_.row(i).clone(), curr_.get());
                map_->insertMapPoint(map_point);
            }
        }
        map_->insertKeyFrame(curr_);
        ref_ = curr_;
    }

    void VisualOdometry::optimizeMap() {
//        remove the hardly seen and no visible points
        for (auto iter = map_->map_points_.begin(); iter != map_->map_points_.end();) {
            if (!curr_->isInFrame(iter->second->pos_)) {
                iter = map_->map_points_.erase(iter);
                continue;
            }
            float match_ratio = float(iter->second->matched_times_) / iter->second->visible_times_;
            if (match_ratio < map_point_erase_ratio_) {
                iter = map_->map_points_.erase(iter);
                continue;
            }
            double angle = getViewAngle(curr_, iter->second);
            if (angle > M_PI / 6.) {
                iter = map_->map_points_.erase(iter);
                continue;
            }
            if (!iter->second->good_) {
//                TODO try triangulate this map point
            }
            iter++;
        }
        if (match_2dkp_index_.size() < 100)
            addMapPoints();
        if (map_->map_points_.size() > 1000) {
//                TODO map is too large, remove some one
            map_point_erase_ratio_ += 0.05;
        } else
            map_point_erase_ratio_ = 0.1;
        cout << "map points: " << map_->map_points_.size() << endl;
    }

    double VisualOdometry::getViewAngle(Frame::Ptr frame, MapPoint::Ptr point) {
        Vector3d n = point->pos_ - frame->getCameraCenter();
        n.normalize();
        return acos(n.transpose() * point->norm_);
    }

    void VisualOdometry::addMapPoints() {
        // add the new map points into map
        vector<bool> matched(keypoints_curr_.size(), false);
        for (int index:match_2dkp_index_)
            matched[index] = true;
        for (int i = 0; i < keypoints_curr_.size(); i++) {
            if (matched[i] == true)
                continue;
            double d = ref_->findDepth(keypoints_curr_[i]);
            if (d < 0)
                continue;
            Vector3d p_world = ref_->camera_->pixel2world(Vector2d(
                    keypoints_curr_[i].pt.x, keypoints_curr_[i].pt.y), curr_->T_c_w_, d);
            Vector3d n = p_world - ref_->getCameraCenter();
            n.normalize();
            MapPoint::Ptr map_point = MapPoint::createMapPoint(
                    p_world, n, descriptors_curr_.row(i).clone(), curr_.get());
            map_->insertMapPoint(map_point);
        }
    }
}
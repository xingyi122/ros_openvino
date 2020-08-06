/*
 * openpose_ros.cpp
 *  Created on: July 15th, 2020
 *      Author: Hilbert Xu
 *   Institute: Mustar Robot
 */

#include <robot_vision_openvino/vino_openpose/openpose_ros.hpp>

// Check for xServer
#include <X11/Xlib.h>

extern "C" {
	double what_time_is_it_now() {
		struct timeval time;
		if (gettimeofday(&time,NULL)) {
			return 0;
		}
		return (double)time.tv_sec + (double)time.tv_usec * .000001;
	}
}

using namespace human_pose_estimation;
using namespace InferenceEngine;


char* weights;

OpenposeROS::OpenposeROS(ros::NodeHandle nh)
: nodeHandle_(nh), imageTransport_(nodeHandle_) {
	ROS_INFO("[OpenposeROS] Node started!");

	if (!readParameters()) {
		// 如果无法从rosparam服务器中读取到对应的模型参数
		// 则发送关闭节点的命令
		ros::requestShutdown();
	}

	init();
}

OpenposeROS::~OpenposeROS() {
	{
		boost::unique_lock<boost::shared_mutex> lockNodeStatus(mutexNodeStatus_);
		isNodeRunning_ = false;
	}
	openposeThread_.join();
}

bool OpenposeROS::readParameters() {
	// Load common parameters.
	nodeHandle_.param("image_view/enable_opencv", viewImage_, true);
	nodeHandle_.param("image_view/wait_key_delay", waitKeyDelay_, 3);
	nodeHandle_.param("image_view/enable_console_output", enableConsoleOutput_, false);

	// Check if Xserver is running on Linux.
  if (XOpenDisplay(NULL)) {
    // Do nothing!
    ROS_INFO("[OpenposeROS] Xserver is running.");
  } else {
    ROS_INFO("[OpenposeROS] Xserver is not running.");
    viewImage_ = false;
  }
	return true;
}

void OpenposeROS::init() {
	ROS_INFO("[OpenposeROS] Initializing...");

	// Initialize weight file for openvino_openpose
	std::string weightsPath;
	std::string weightsModel;

	nodeHandle_.param("openpose_model/weight_file/name", weightsModel, std::string("human-pose-estimation.xml"));
	nodeHandle_.param("weights_path", weightsPath, std::string("/default"));

	weightsPath += weightsModel;
	weights = new char[weightsPath.length() + 1];
	strcpy(weights, weightsPath.c_str());

	// Set up inference engine
	OpenposeROS::setUpInferenceEngine();

	// start openpose thread
	openposeThread_ = std::thread(&OpenposeROS::openpose, this);

	// Initialize publisher and subscriber
	std::string cameraTopicName;
	int cameraQueueSize;
	std::string estimationImageTopicName;
  int estimationImageQueueSize;
	bool estimationImageLatch;
  std::string subControlTopicName;
  int subControlQueueSize;
  std::string pubControlTopicName;
  int pubControlQueueSize;
  bool pubControlLatch;

	nodeHandle_.param("subscribers/camera_reading/topic", cameraTopicName, std::string("/astra/rgb/image_raw"));
  nodeHandle_.param("subscribers/camera_reading/queue_size", cameraQueueSize, 1);
	nodeHandle_.param("publishers/estimation_image/topic", estimationImageTopicName, std::string("estimation_image"));
  nodeHandle_.param("publishers/estimation_image/queue_size", estimationImageQueueSize, 1);
  nodeHandle_.param("publishers/estimation_image/latch", estimationImageLatch, true);
  nodeHandle_.param("subscribers/control_node/topic", subControlTopicName, std::string("/control_to_vision"));
  nodeHandle_.param("subscribers/control_node/queue_size", subControlQueueSize, 1);
  nodeHandle_.param("publisher/control_node/topic", pubControlTopicName, std::string("/vision_to_control"));
  nodeHandle_.param("publisher/control_node/queue_size", pubControlQueueSize, 1);
  nodeHandle_.param("publisher/control_node/latch", pubControlLatch, false);
  
	imageSubscriber_ = imageTransport_.subscribe(cameraTopicName, cameraQueueSize, &OpenposeROS::cameraCallback, this);
	estimationImagePublisher_ =
      nodeHandle_.advertise<sensor_msgs::Image>(estimationImageTopicName, estimationImageQueueSize, estimationImageLatch);
  controlSubscriber_ = nodeHandle_.subscribe(subControlTopicName, subControlQueueSize, &OpenposeROS::controlCallback, this);
  controlPublisher_ = 
      nodeHandle_.advertise<robot_control_msgs::Feedback>(pubControlTopicName, 1, pubControlLatch);

	// Action servers
	std::string checkForHumanPosesActionName;
	nodeHandle_.param("action/camera_reading/topic", checkForHumanPosesActionName, std::string("check_for_human_pose"));
	checkForHumanPosesActionServer_.reset(new CheckForHumanPosesActionServer(nodeHandle_, checkForHumanPosesActionName, false));

  checkForHumanPosesActionServer_->registerGoalCallback(boost::bind(&OpenposeROS::checkForHumanPosesActionGoalCB, this));

  checkForHumanPosesActionServer_->registerPreemptCallback(boost::bind(&OpenposeROS::checkForHumanPosesActionPreemptCB, this));

  checkForHumanPosesActionServer_->start();
}

void OpenposeROS::controlCallback(const robot_control_msgs::Mission msg) {
	// 接收来自control节点的消息
	if (msg.target == "human_pose") {
		// 如果目标是human pose
		if (msg.action == "detect" && msg.attributes.human.gesture != "") {
			// 有检测目标 --> 检测特定姿态
			detectSpecificPose_ = true;
			// 记录目标姿态
			targetPose_ = msg.attributes.human.gesture;
			// 开始进行姿态估计
			startEstimateFlag_ = true;
			// 允许发布检测结果信息
			pubMessageFlag_ = true;
		}
		else if (msg.action == "estimate") {
			// 没有检测目标 --> 返回每帧图像中所有人体骨架的关键点以及姿态
			detectSpecificPose_ = false;
			// 开始进行姿态估计
			startEstimateFlag_ = true;
			// 允许发布检测结果信息
			pubMessageFlag_ = true;
		}
		else if (msg.action.find("stop")){
			// stop --> 停止对接收到的图像进行推理
			startEstimateFlag_ = false;	
		}
	}
}

void OpenposeROS::cameraCallback(const sensor_msgs::ImageConstPtr& msg) {
	ROS_DEBUG("[OpenposeROS] USB image received");

	cv_bridge::CvImagePtr cam_image;

	// 将ros格式图像数据转换为cv_bridge::CvImagePtr格式
	try {
		cam_image = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
	} catch (cv_bridge::Exception& e) {
		ROS_ERROR("cv_bridge exception: %s", e.what());
		return;
	}
	// 如果完成了图像数据的转换，则通过施加独立锁的方式来将图像数据写入临界区
	// 同时更新图像状态
	if (cam_image) {
		{
			// 为mutexImageCallback_加写锁(unique_lock), 并写入图像数据
			boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexImageCallback_);
			imageHeader_ = msg->header;
			camImageCopy_ = cam_image->image.clone();
		}
		{
			// 为mutexImageStatus加写锁, 并更新当前图像状态，表示已经接收到图像数据
			boost::unique_lock<boost::shared_mutex> lockImageStatus(mutexImageStatus_);
			imageStatus_ = true;
		}
		// 记录相机参数(图像长宽)
		frameWidth_ = cam_image->image.size().width;
    frameHeight_ = cam_image->image.size().height;
	}
	return;
}

void OpenposeROS::checkForHumanPosesActionGoalCB() {
	ROS_DEBUG("[OpenposeROS] Start check for human poses action");

	boost::shared_ptr<const robot_vision_msgs::CheckForHumanPosesGoal> imageActionPtr = checkForHumanPosesActionServer_->acceptNewGoal();
	sensor_msgs::Image imageAction = imageActionPtr->image;

	cv_bridge::CvImagePtr cam_image;

	try {
    cam_image = cv_bridge::toCvCopy(imageAction, sensor_msgs::image_encodings::BGR8);
  } catch (cv_bridge::Exception& e) {
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }
	if (cam_image) {
    {
      boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexImageCallback_);
      camImageCopy_ = cam_image->image.clone();
    }
    {
      boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexActionStatus_);
      actionId_ = imageActionPtr->id;
    }
    {
      boost::unique_lock<boost::shared_mutex> lockImageStatus(mutexImageStatus_);
      imageStatus_ = true;
    }
    frameWidth_ = cam_image->image.size().width;
    frameHeight_ = cam_image->image.size().height;
  }
  return;
}

void OpenposeROS::checkForHumanPosesActionPreemptCB() {
  ROS_DEBUG("[OpenposeROS] Preempt check for human poses action.");
  checkForHumanPosesActionServer_->setPreempted();
}

bool OpenposeROS::isCheckingForHumanPoses() const {
  return (ros::ok() && checkForHumanPosesActionServer_->isActive() && !checkForHumanPosesActionServer_->isPreemptRequested());
}

void OpenposeROS::showImageCV(cv::Mat image) {
	cv::imshow("OpenPose ROS on CPU", image);
}

// 抓取线程，从临界区中抓取数据，并写入缓存区
void* OpenposeROS::fetchInThread() {
	{
		// 为mutexImageCallback_对象加读锁，获取读取临界区的权限
		boost::shared_lock<boost::shared_mutex> lock(mutexImageCallback_);
		// 临界区读取函数
		MatImageWithHeader_ imageWithHeader = getMatImageWithHeader();
		// 将临界区的数据写入缓存区
		buff_[buffIndex_] = imageWithHeader.image.clone();
		headerBuff_[buffIndex_] = imageWithHeader.header;
		buffId_[buffIndex_] = actionId_;
	}
	//! TODO
	//! 根据OpenVINO的推理引擎要求，对缓存区内的图像进行预处理，(bgr8 -> rgb, resize)
}

void* OpenposeROS::displayInThread(void* ptr) {
	// 显示缓存区中的图片数据
  OpenposeROS::showImageCV(buff_[(buffIndex_ + 1) % 3]);
  int c = cv::waitKey(waitKeyDelay_);
	// 判断waitKey, 是否退出demo
  if (c != -1) c = c % 256;
  if (c == 27) {
    demoDone_ = 1;
    return 0;
  } 
  return 0;
}

void* OpenposeROS::estimateInThread() {
	//! TODO
	// build estimate thread 
	// 注意加入内存回收机制，free掉使用过的vector等容器
	std::vector<HumanPose> poses;
	// preprocess image
	estimator.reshape(buff_[(buffIndex_+2)%3]);
	// load image
	estimator.frameToBlobCurr(buff_[(buffIndex_+2)%3]);
	// inference current image
	estimator.startCurr();
	// Waiting for current inference
	while (true) {
		if (estimator.readyCurr()) {
			break;
		}
	}
	//! waiting for inference end...
	// generate poses keypoints
	poses = estimator.postprocessCurr();
	// rendering keypoints
	renderHumanPose(poses, buff_[(buffIndex_+2)%3]);
}

// 初始化推理引擎
void OpenposeROS::setUpInferenceEngine() {
	ROS_INFO("[OpenposeROS] InferenceEngine: %s", InferenceEngine::GetInferenceEngineVersion()->buildNumber);
  estimator.initializeAll(weights, "CPU", false);
}


void OpenposeROS::openpose() {
	const auto wait_duration = std::chrono::milliseconds(2000);
	while (!getImageStatus()) {
		printf("Waiting for image.\n");
		if (!isNodeRunning_) {
			return;
		}
		std::this_thread::sleep_for(wait_duration);
	}

	std::thread estimate_thread;
	std::thread fetch_thread;

	srand(22222222);

	{
		// 初始化缓存区内数据, 用将当前帧填满缓存区
		boost::shared_lock<boost::shared_mutex> lock(mutexImageCallback_);
		MatImageWithHeader_ imageWithHeader = getMatImageWithHeader();
		cv::Mat ros_img = imageWithHeader.image.clone();
		buff_[0] = ros_img;
    headerBuff_[0] = imageWithHeader.header;
	}
	buff_[1] = buff_[0].clone();
	buff_[2] = buff_[0].clone();
	headerBuff_[1] = headerBuff_[0];
	headerBuff_[2] = headerBuff_[0];

	int count = 0;

	// 初始化显示图像的窗口
	if (!demoPrefix_ && viewImage_) {
		cv::namedWindow("OpenPose ROS on CPU", cv::WINDOW_NORMAL);
		cv::moveWindow("OpenPose ROS on CPU", 0, 0);
		cv::resizeWindow("OpenPose ROS on CPU", 640, 480);
	}
	
	demoTime_ = what_time_is_it_now();

	while (!demoDone_) {
		// buffIndex_在(0, 1, 2)间循环
		buffIndex_ = (buffIndex_ + 1) % 3;
		// 为fetchInThread函数生成一个线程
		fetch_thread = std::thread(&OpenposeROS::fetchInThread, this);
		// 为estimateInThread函数生成一个线程
		estimate_thread = std::thread(&OpenposeROS::estimateInThread, this);
		// 计算fps和时间
		fps_ = 1. /(what_time_is_it_now() - demoTime_);
		demoTime_ = what_time_is_it_now();

		// 显示检测图片
		if (viewImage_) {
			displayInThread(0);
		} 
		// 发布识别结果
		// publishInThread();

		// 等待fetch_thread 和 estimate_thread完成
		fetch_thread.join();
		estimate_thread.join();
		// 计数
		++count;
		// 如果节点停止运行，终止demo
		if (!isNodeRunning()) {
			demoDone_ = true;
		}
	}
}

MatImageWithHeader_ OpenposeROS::getMatImageWithHeader() {
	cv::Mat rosImage = camImageCopy_.clone();
	MatImageWithHeader_ imageWithHeader = {.image=rosImage, .header=imageHeader_};
	return imageWithHeader;
}

bool OpenposeROS::getImageStatus(void) {
	boost::shared_lock<boost::shared_mutex> lock(mutexImageStatus_);
	return imageStatus_;
}

bool OpenposeROS::isNodeRunning(void) {
	boost::shared_lock<boost::shared_mutex> lock(mutexNodeStatus_);
	return isNodeRunning_;
}




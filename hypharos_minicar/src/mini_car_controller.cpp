/*
Copyright (c) 2017, ChanYuan KUO, YoRu LU,
latest editor: HaoChih, LIN
All rights reserved. (Hypha ROS Workshop)

This file is part of hypha_racecar package.

hypha_racecar is free software: you can redistribute it and/or modify
it under the terms of the GNU LESSER GENERAL PUBLIC LICENSE as published
by the Free Software Foundation, either version 3 of the License, or
any later version.

hypha_racecar is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU LESSER GENERAL PUBLIC LICENSE for more details.

You should have received a copy of the GNU LESSER GENERAL PUBLIC LICENSE
along with hypha_racecar.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include "ros/ros.h"
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Twist.h>
#include <tf/transform_listener.h>
#include <tf/transform_datatypes.h>
#include "nav_msgs/Path.h"
#include <nav_msgs/Odometry.h>
#include <visualization_msgs/Marker.h>
#include "std_msgs/Int32.h"

#define PI 3.14159265358979
int start_loop_flag = 0;
int start_speed = 1550;
double speed_angle;
int car_stop = 0;
int flag = 0;
enum Stage_step{
    Stop = 0,
    Start_step,
    Row_step,
    Straight_step,
    Finall_step,
}state = Stop;

class L1Controller
{
    public:
        std_msgs::Int32 i;   //记录目标点个数
        L1Controller();
        void initMarker();
        bool isForwardWayPt(const geometry_msgs::Point& wayPt, const geometry_msgs::Pose& carPose);
        bool isWayPtAwayFromLfwDist(const geometry_msgs::Point& wayPt, const geometry_msgs::Point& car_pos);
        double getYawFromPose(const geometry_msgs::Pose& carPose);        
        double getEta(const geometry_msgs::Pose& carPose);
        double getCar2GoalDist();
        double getL1Distance(const double& _Vcmd);
        double getSteeringAngle(double eta);
        double getGasInput(const float& current_v);
        geometry_msgs::Point get_odom_car2WayPtVec(const geometry_msgs::Pose& carPose);
        void Judge_turn(geometry_msgs::Pose carPose,nav_msgs::Path map_path);
    private:
        ros::NodeHandle n_;
        ros::Subscriber odom_sub, path_sub, goal_sub;
        ros::Publisher pub_, marker_pub;
        ros::Timer timer1, timer2;
        tf::TransformListener tf_listener;
	ros::Publisher R_goal;

        visualization_msgs::Marker points, line_strip, goal_circle;
        geometry_msgs::Twist cmd_vel;
        geometry_msgs::Point odom_goal_pos;
        nav_msgs::Odometry odom;
        nav_msgs::Path map_path, odom_path;

        double L, Lfw, Lrv, Vcmd, lfw, lrv, steering, u, v;
        double Gas_gain, baseAngle, Angle_gain, goalRadius;
        int controller_freq, baseSpeed;
        bool foundForwardPt, goal_received, goal_reached;

        void odomCB(const nav_msgs::Odometry::ConstPtr& odomMsg);
        void pathCB(const nav_msgs::Path::ConstPtr& pathMsg);
        void goalCB(const geometry_msgs::PoseStamped::ConstPtr& goalMsg);
        void goalReachingCB(const ros::TimerEvent&);
        void controlLoopCB(const ros::TimerEvent&);
        
}; // end of class


L1Controller::L1Controller()
{
    //Private parameters handler
    ros::NodeHandle pn("~");

    //Car parameter
    pn.param("L", L, 0.33);
    pn.param("Lrv", Lrv, 10.0);
    pn.param("Vcmd", Vcmd, 1.0);
    pn.param("lfw", lfw, 0.165);
    pn.param("lrv", lrv, 10.0);

    //Controller parameter
    pn.param("controller_freq", controller_freq, 20);
    pn.param("AngleGain", Angle_gain, -1.0);
    pn.param("GasGain", Gas_gain, 1.0);
    pn.param("baseSpeed", baseSpeed, 1470);
    pn.param("baseAngle", baseAngle, 90.0);

    //Publishers and Subscribers
    odom_sub = n_.subscribe("/odom", 1, &L1Controller::odomCB, this);
    path_sub = n_.subscribe("/move_base_node/NavfnROS/plan", 1, &L1Controller::pathCB, this);
    goal_sub = n_.subscribe("/move_base_simple/goal", 1, &L1Controller::goalCB, this);
    marker_pub = n_.advertise<visualization_msgs::Marker>("car_path", 10);
    pub_ = n_.advertise<geometry_msgs::Twist>("car/cmd_vel", 1);

    R_goal=n_.advertise<std_msgs::Int32>("int32",1);      //创建发布的topic

    //Timer
    timer1 = n_.createTimer(ros::Duration((1.0)/controller_freq), &L1Controller::controlLoopCB, this); // Duration(0.05) -> 20Hz
    timer2 = n_.createTimer(ros::Duration((0.5)/controller_freq), &L1Controller::goalReachingCB, this); // Duration(0.05) -> 20Hz

    //Init variables
    Lfw = goalRadius = getL1Distance(Vcmd);
    foundForwardPt = false;
    goal_received = false;
    goal_reached = false;
    cmd_vel.linear.x = 1500; // 1500 for stop
    cmd_vel.angular.z = baseAngle;

    i.data=1;   //goal的位置

    //Show info
    ROS_INFO("[param] baseSpeed: %d", baseSpeed);
    ROS_INFO("[param] baseAngle: %f", baseAngle);
    ROS_INFO("[param] AngleGain: %f", Angle_gain);
    ROS_INFO("[param] Vcmd: %f", Vcmd);
    ROS_INFO("[param] Lfw: %f", Lfw);

    //Visualization Marker Settings
    initMarker();
    car_stop = 0;
}



void L1Controller::initMarker()
{
    points.header.frame_id = line_strip.header.frame_id = goal_circle.header.frame_id = "odom";
    points.ns = line_strip.ns = goal_circle.ns = "Markers";
    points.action = line_strip.action = goal_circle.action = visualization_msgs::Marker::ADD;
    points.pose.orientation.w = line_strip.pose.orientation.w = goal_circle.pose.orientation.w = 1.0;
    points.id = 0;
    line_strip.id = 1;
    goal_circle.id = 2;

    points.type = visualization_msgs::Marker::POINTS;
    line_strip.type = visualization_msgs::Marker::LINE_STRIP;
    goal_circle.type = visualization_msgs::Marker::CYLINDER;
    // POINTS markers use x and y scale for width/height respectively
    points.scale.x = 0.2;
    points.scale.y = 0.2;

    //LINE_STRIP markers use only the x component of scale, for the line width
    line_strip.scale.x = 0.1;

    goal_circle.scale.x = goalRadius;
    goal_circle.scale.y = goalRadius;
    goal_circle.scale.z = 0.1;

    // Points are green
    points.color.g = 1.0f;
    points.color.a = 1.0;

    // Line strip is blue
    line_strip.color.b = 1.0;
    line_strip.color.a = 1.0;

    //goal_circle is yellow
    goal_circle.color.r = 1.0;
    goal_circle.color.g = 1.0;
    goal_circle.color.b = 0.0;
    goal_circle.color.a = 0.5;
}

void L1Controller::odomCB(const nav_msgs::Odometry::ConstPtr& odomMsg)
{
    odom = *odomMsg;
}


void L1Controller::pathCB(const nav_msgs::Path::ConstPtr& pathMsg)
{
    map_path = *pathMsg;
}


void L1Controller::goalCB(const geometry_msgs::PoseStamped::ConstPtr& goalMsg)
{
    try
    {
        geometry_msgs::PoseStamped odom_goal;
        tf_listener.transformPose("odom", ros::Time(0) , *goalMsg, "map" ,odom_goal);
        odom_goal_pos = odom_goal.pose.position;
        goal_received = true;
        goal_reached = false;

        /*Draw Goal on RVIZ*/
        goal_circle.pose = odom_goal.pose;
        marker_pub.publish(goal_circle);
    }
    catch(tf::TransformException &ex)
    {
        ROS_ERROR("%s",ex.what());
        ros::Duration(1.0).sleep();
    }
}

double L1Controller::getYawFromPose(const geometry_msgs::Pose& carPose)
{
    float x = carPose.orientation.x;
    float y = carPose.orientation.y;
    float z = carPose.orientation.z;
    float w = carPose.orientation.w;

    double tmp,yaw;
    tf::Quaternion q(x,y,z,w);
    tf::Matrix3x3 quaternion(q);
    quaternion.getRPY(tmp,tmp, yaw);

    return yaw;
}

bool L1Controller::isForwardWayPt(const geometry_msgs::Point& wayPt, const geometry_msgs::Pose& carPose)
{
    float car2wayPt_x = wayPt.x - carPose.position.x;
    float car2wayPt_y = wayPt.y - carPose.position.y;
    double car_theta = getYawFromPose(carPose);

    float car_car2wayPt_x = cos(car_theta)*car2wayPt_x + sin(car_theta)*car2wayPt_y;
    float car_car2wayPt_y = -sin(car_theta)*car2wayPt_x + cos(car_theta)*car2wayPt_y;

    if(car_car2wayPt_x >0) /*is Forward WayPt*/
        return true;
    else
        return false;
}


bool L1Controller::isWayPtAwayFromLfwDist(const geometry_msgs::Point& wayPt, const geometry_msgs::Point& car_pos)
{
    double dx = wayPt.x - car_pos.x;
    double dy = wayPt.y - car_pos.y;
    double dist = sqrt(dx*dx + dy*dy);

    if(dist < Lfw)
        return false;
    else if(dist >= Lfw)
        return true;
}

geometry_msgs::Point L1Controller::get_odom_car2WayPtVec(const geometry_msgs::Pose& carPose)
{
    geometry_msgs::Point carPose_pos = carPose.position;
    double carPose_yaw = getYawFromPose(carPose);
    geometry_msgs::Point forwardPt;
    geometry_msgs::Point odom_car2WayPtVec;
    foundForwardPt = false;

    if(!goal_reached){
        // printf("************************************\n");
        // printf("size:%d",map_path.poses.size());
        for(int i =0; i< map_path.poses.size(); i++)
        {
             
            geometry_msgs::PoseStamped map_path_pose = map_path.poses[i];//获取路径点
            // printf("map_path.x=%.5f\n",map_path_pose.pose.position.x);
            // printf("map_path.y=%.5f\n",map_path_pose.pose.position.y);
            geometry_msgs::PoseStamped odom_path_pose;
            try
            {
                tf_listener.transformPose("odom", ros::Time(0) , map_path_pose, "map" ,odom_path_pose);
                geometry_msgs::Point odom_path_wayPt = odom_path_pose.pose.position;
                bool _isForwardWayPt = isForwardWayPt(odom_path_wayPt,carPose);

                if(_isForwardWayPt)
                {
                    bool _isWayPtAwayFromLfwDist = isWayPtAwayFromLfwDist(odom_path_wayPt,carPose_pos);
                    if(_isWayPtAwayFromLfwDist)
                    {
                        forwardPt = odom_path_wayPt;
                        foundForwardPt = true;
                        break;
                    }
                }
            }
            catch(tf::TransformException &ex)
            {
                ROS_ERROR("%s",ex.what());
                ros::Duration(1.0).sleep();
            }
        }
        
    }
    else if(goal_reached)
    {
        forwardPt = odom_goal_pos;
        foundForwardPt = false;
        //ROS_INFO("goal REACHED!");
    }

    /*Visualized Target Point on RVIZ*/
    /*Clear former target point Marker*/
    points.points.clear();
    line_strip.points.clear();
    
    if(foundForwardPt && !goal_reached)
    {
        points.points.push_back(carPose_pos);
        points.points.push_back(forwardPt);
        line_strip.points.push_back(carPose_pos);
        line_strip.points.push_back(forwardPt);
    }

    marker_pub.publish(points);
    marker_pub.publish(line_strip);
    
    odom_car2WayPtVec.x = cos(carPose_yaw)*(forwardPt.x - carPose_pos.x) + sin(carPose_yaw)*(forwardPt.y - carPose_pos.y);
    odom_car2WayPtVec.y = -sin(carPose_yaw)*(forwardPt.x - carPose_pos.x) + cos(carPose_yaw)*(forwardPt.y - carPose_pos.y);
    return odom_car2WayPtVec;
}


double L1Controller::getEta(const geometry_msgs::Pose& carPose)
{
    geometry_msgs::Point odom_car2WayPtVec = get_odom_car2WayPtVec(carPose);

    double eta = atan2(odom_car2WayPtVec.y,odom_car2WayPtVec.x);
    return eta;
}


double L1Controller::getCar2GoalDist()
{
    geometry_msgs::Point car_pose = odom.pose.pose.position;
    double car2goal_x = odom_goal_pos.x - car_pose.x;
    double car2goal_y = odom_goal_pos.y - car_pose.y;

    double dist2goal = sqrt(car2goal_x*car2goal_x + car2goal_y*car2goal_y);

    return dist2goal;
}

double L1Controller::getL1Distance(const double& _Vcmd)
{
    double L1 = 0;
    if(_Vcmd < 1.34)
        L1 = 3 / 3.0;
    else if(_Vcmd > 1.34 && _Vcmd < 5.36)
        L1 = _Vcmd*2.24 / 3.0;
    else
        L1 = 12 / 3.0;
    return L1;
}

double L1Controller::getSteeringAngle(double eta)
{
    double steeringAnge = -atan2((L*sin(eta)),(Lfw/2+lfw*cos(eta)))*(180.0/PI);
    //ROS_INFO("Steering Angle = %.2f", steeringAnge);
    return steeringAnge;
}

double L1Controller::getGasInput(const float& current_v)
{
    double u = (Vcmd - current_v)*Gas_gain;
    //ROS_INFO("velocity = %.2f\tu = %.2f",current_v, u);
    return u;
}


void L1Controller::goalReachingCB(const ros::TimerEvent&)
{

    if(goal_received)
    {
        double car2goal_dist = getCar2GoalDist();
        if(car2goal_dist < goalRadius)
        { 
           // ROS_INFO("11111%.2f",goalRadius);
           //从这里发布一条信息表明到达终点，send_goal接收到这条消息，发布下一个目标点。
           //pub(i)
           //i++
            i.data++;
            R_goal.publish(i);	         

            goal_reached = true;
            goal_received = false;
            car_stop = 100;
            ROS_INFO("Goal Reached !");
        }
    }
}

void Judge_turn(geometry_msgs::Pose carPose,nav_msgs::Path map_path)
{   
    // geometry_msgs::PoseStamped odom_path_pose;
    // for(int i=0;i<50;i++)
    // {
    //     geometry_msgs::PoseStamped map_path_pose = map_path.poses[i];//获取路径点
    //     tf_listener.transformPose("odom", ros::Time(0) , map_path_pose, "map" ,odom_path_pose);
    //     // geometry_msgs::Point odom_path_wayPt = odom_path_pose.pose.position;
    //     // float flog = abs(carPose.position.y-odom_path_pose.pose.position.y);
    //     // if(flog>0.1)
    //     // {
    //     //     state = Row_step;
    //     //     break;
    //     //     // cmd_vel.linear.x = 1570;
    //     // }
    //     // else
    //     // {
    //     //     cmd_vel.linear.x = baseSpeed;   
    //     // }
    // }
}



void L1Controller::controlLoopCB(const ros::TimerEvent&)
{

    geometry_msgs::Pose carPose = odom.pose.pose;
    geometry_msgs::Twist carVel = odom.twist.twist;
    // cmd_vel.linear.x = 1500;
    cmd_vel.angular.z = baseAngle;

    if(goal_received)
    {

        /*Estimate Steering Angle*/
        double eta = getEta(carPose);  
        if(foundForwardPt)
        {
            cmd_vel.angular.z = baseAngle + getSteeringAngle(eta)*Angle_gain;
            /*Estimate Gas Input*/
            if(!goal_reached)
            {
                if(cmd_vel.linear.x<baseSpeed)
                {
                    //初始为1500，停止的功率
                    if(state == Stop)
                    {
                        cmd_vel.linear.x = start_speed;
                        state = Start_step;
                    }
                    //控制起步的步长
                    if(state == Start_step)
                    {
                        cmd_vel.linear.x = cmd_vel.linear.x + 1.0;
                    }
                    //控制转弯时x方向的步长
                    if(state == Row_step)
                    {
                        cmd_vel.linear.x = cmd_vel.linear.x + 4.0;
                        if(abs(cmd_vel.angular.z-90) < 5)
                        {
                            state = Straight_step;
                        }
                    }
                    //当转弯完成时，还没有达到最大的pwm时，控制直线上的步长
                    if(state == Straight_step)
                    {
                        cmd_vel.linear.x = cmd_vel.linear.x +5.0;
                    }

                    if(state == Finall_step)
                    {
                        
                    }
                    
                    // if(abs(cmd_vel.angular.z-90)>35)
                    //     cmd_vel.linear.x = 1568;
                    ROS_INFO("baseSpeed = %.2f\tSteering angle = %.2f",cmd_vel.linear.x,cmd_vel.angular.z);
                }
                else
                {
                    if(cmd_vel.linear.x >= baseSpeed)
                    {
                        state = Straight_step;
                    }
                    //ROS_INFO("!goal_reached");
                    // double u = getGasInput(carVel.linear.x);                   
                    // cmd_vel.linear.x = baseSpeed + PIDCal(&pid_speed,u);
                    // if(abs(cmd_vel.angular.z-90)>35)
                    //     cmd_vel.linear.x = 1568;
                    // else
                    //     cmd_vel.linear.x = baseSpeed;

                    //写一个判断状态的函数   可以写一个线程




                    
                    // if(state == Row_step)
                    // {
                    //     cmd_vel.linear.x = 1570;
                    //     state = Straight_step;
                    // }

                    if(state == Straight_step)
                    {
                        if(cmd_vel.linear.x < baseSpeed)
                        {
                            cmd_vel.linear.x = cmd_vel.linear.x + 1.0;
                        }
                        else
                        {
                            cmd_vel.linear.x = baseSpeed;
                        }
                        geometry_msgs::PoseStamped odom_path_pose;
                        for(int i=0;i<60;i++)
                        {
                            geometry_msgs::PoseStamped map_path_pose = map_path.poses[i];//获取路径点
                            tf_listener.transformPose("odom", ros::Time(0) , map_path_pose, "map" ,odom_path_pose);
                            geometry_msgs::Point odom_path_wayPt = odom_path_pose.pose.position;
                            float flog = abs(carPose.position.y-odom_path_pose.pose.position.y);
                            if(flog>0.5)
                            {
                                cmd_vel.linear.x = 1550;
                                state = Row_step;
                                break;
                            }
                        }
                    }

                    if(state == Finall_step)
                    {
                        
                    }      
                        
                }
                    ROS_INFO("Gas = %.2f\tSteering angle = %.2f",cmd_vel.linear.x,cmd_vel.angular.z);
                }  
            }
        }
    if(car_stop > 0)
    {
        start_loop_flag = 0;
        if(carVel.linear.x > 0)
        {

            cmd_vel.linear.x = 1300; //反向刹车
            pub_.publish(cmd_vel);
           // for(int i=0;i<20;i++)
           // {
           //     pub_.publish(cmd_vel);
           //     sleep(0.1);
           //     ROS_INFO("cat stop cmd_vel= %f",cmd_vel.linear.x);
           // }
            
        }
        else
        {
            car_stop = 0;
            cmd_vel.linear.x = 1500;
            pub_.publish(cmd_vel);

            //ROS_INFO("cmd_vel= %f",cmd_vel.linear.x);
        }
    }
    else
    {
        pub_.publish(cmd_vel);
        car_stop = 0;
        //ROS_INFO("car run cmd_vel= %f",cmd_vel.linear.x);
    }

    pub_.publish(cmd_vel);
    if(i.data==1)
        R_goal.publish(i);

}


/*****************/
/* MAIN FUNCTION */
/*****************/
int main(int argc, char **argv)
{
    //Initiate ROS
    ros::init(argc, argv, "mini_car_controller");
    L1Controller controller;
    ros::spin();
    return 0;
}

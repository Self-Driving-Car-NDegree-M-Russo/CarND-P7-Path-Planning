#include <uWS/uWS.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "helpers.h"
#include "json.hpp"
#include "spline.h"
#include "cost.h"
#include "vehicle.h"

// for convenience
using nlohmann::json;
using std::string;
using std::vector;
using std::map;

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  std::ifstream in_map_(map_file_.c_str(), std::ifstream::in);

  string line;
  while (getline(in_map_, line)) {
    std::istringstream iss(line);
    double x;
    double y;
    float s;
    float d_x;
    float d_y;
    iss >> x;
    iss >> y;
    iss >> s;
    iss >> d_x;
    iss >> d_y;
    map_waypoints_x.push_back(x);
    map_waypoints_y.push_back(y);
    map_waypoints_s.push_back(s);
    map_waypoints_dx.push_back(d_x);
    map_waypoints_dy.push_back(d_y);
  }

  // Starting lane identifier
  // Lane can be {0,1,2}, left to right
  // By default the vehicle starts in the middle lane
  int lane = 1;

  // Reference velocity
  // Starting from 0 the vehicle will accelerate/decelerate
  double ref_vel = 0;

  // EGO STATE
  // Initialized at KL
  string ego_state = "KL";

  // Initial acceleration phase
  // while the vehicle is accelerating for the first time (up to 50 mph) we
  // will assume to stay in the same lane
  bool init_acc_over = false;

  // lambda function called on message from sim
  h.onMessage([&ref_vel, &max_s, &lane, &ego_state, &init_acc_over,
               &map_waypoints_x,&map_waypoints_y,&map_waypoints_s,
               &map_waypoints_dx,&map_waypoints_dy]
              (uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
               uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);

        string event = j[0].get<string>();

        if (event == "telemetry") {
          // j[1] is the data JSON object

          // ===================================================================
          // MESSAGE PARSING
          // Main car's localization Data
          double car_x = j[1]["x"];
          double car_y = j[1]["y"];
          double car_s = j[1]["s"];
          double car_d = j[1]["d"];
          double car_yaw = j[1]["yaw"];
          double car_speed = j[1]["speed"];

          std::cout << "****************************************** "<< std::endl;
          // Previous path data given to the Planner and NOT yet executed by
          // the car
          auto previous_path_x = j[1]["previous_path_x"];
          auto previous_path_y = j[1]["previous_path_y"];
          // Previous path's end s and d values
          double end_path_s = j[1]["end_path_s"];
          double end_path_d = j[1]["end_path_d"];

          // Sensor Fusion Data, a list of all other cars on the same side
          //   of the road.
          auto sensor_fusion = j[1]["sensor_fusion"];

          json msgJson;
          
          // ===================================================================
          // INITIALIZATION
          // Vectors to pass as next trajectory
          vector<double> next_x_vals;
          vector<double> next_y_vals;

          // Parameters to be used in following calculations:
          // Lane width: lane considered 4 meters wide
          float lane_width = 4;
          // Sampling interval
          float delta_t = 0.02;

          // Size of what's left of the previos path
          // This will depend on how much of the previous planned path the car
          // has covered in the 0.02 seconds between simulator steps, that in
          // turn is a function of the car's speed
          int prev_size = previous_path_x.size();

          // In case we have something from previous run, change car_s to be at
          // the end of the previous path
          if (prev_size>0){
            car_s = end_path_s;
          }

          // Maps to be filled with the vehicles in the scene and their possible trajectories
          map<int, Vehicle> vehicles;
          map<int ,vector<Vehicle> > predictions;

          // ===================================================================
          // CREATION OF AN EGO VEHICLE OBJECT
          // Definition of the ego lane
          int sensed_ego_lane;

          if ((car_d >= 0.0) && (car_d < lane_width)){
            sensed_ego_lane = 0;
          } else if ((car_d >= lane_width) && (car_d < 2*lane_width)){
            sensed_ego_lane = 1;
          } else {
            sensed_ego_lane = 2;
          }

          // Instantiation of the ego vehicle object
          // NOTE1: speed from msg is in mph
          // NOTE2: vehicle's acceleration is assumed = 0
          Vehicle ego_vehicle = Vehicle(sensed_ego_lane,car_s,car_speed*0.44704,0);
          
          // Setting of the current state
          ego_vehicle.state = ego_state;
          
          // Setting of the goals in terms of distance to reach and speed to maintain
          ego_vehicle.goal_s = max_s;
          ego_vehicle.target_speed = ref_vel*0.44704;

          // ===================================================================
          // FINITE STATE MACHINE
          
          // Get in it only if the initial acceleration phase is over
          if (init_acc_over == true){
            
            // 1. Create maps for vehicles and trajectories
            
            // Vehicle counter 
            int vehicles_added = 0;
            
            for (int l = 0; l < sensor_fusion.size(); ++l) {
              
              // Create a vehicle object for every vehicle in sensor fusion
              // Read sensed coordinates/velocity
              float sensed_vx  = sensor_fusion[l][3];
              float sensed_vy  = sensor_fusion[l][4];
              float sensed_s  = sensor_fusion[l][5];
              float sensed_d  = sensor_fusion[l][6];
              
              // Define a lane for the vehicle
              int sensed_lane;

              if ((sensed_d >= 0.0) && (sensed_d < lane_width)){
                sensed_lane = 0;
              } else if ((sensed_d >= lane_width) && (sensed_d < 2*lane_width)){
                sensed_lane = 1;
              } else {
                sensed_lane = 2;
              }
              
              // Calculate the magnitude of the speed
              float sensed_speed = sqrt(sensed_vx*sensed_vx + sensed_vy*sensed_vy);

              // Create vehicle object from current data
              Vehicle vehicle = Vehicle(sensed_lane,sensed_s,sensed_speed,0);
              
              // Set a state, assuming constant speed 
              vehicle.state = "CS";
              
              // Insert vehicles in the map
              vehicles_added += 1;
              vehicles.insert(std::pair<int,Vehicle>(vehicles_added,vehicle));

              // Create predicted trajectory
              // NOTE: default horizon = 2 s
              vector<Vehicle> preds = vehicle.generate_predictions();
              predictions[vehicles_added] = preds;
            }
            
            // 2. Change Ego state based on predictions
            
            vector<Vehicle> trajectory = ego_vehicle.choose_next_state(predictions);
            ego_vehicle.realize_next_state(trajectory);

            ego_state = ego_vehicle.state;
            std::cout << "Ego Vehicle state after predictions: "<< ego_state << std::endl;
          }
          
          // ===================================================================
          // UPDATE REF VELOCITY in case of KL state
          // In case we can stay in this lane let's check if we can accelerate
          // or we'd better slow down
          
          // *************************************
          // DUMMY
          ego_state = "KL";
          // *************************************
          
          if (ego_state == "KL"){
            // First of all let's check if there's a chance of getting too close
            // to other cars while keeping this lane, and adapt velocity
            bool too_close = false;

            // Counter
            int i = 0;
            
            // Loop over sensor_fusionm
            // until you find a vehicle which is in the same lane as ego AND too close
            while (too_close == false){
              
              i++;
              float d = sensor_fusion[i][6];

              // check if there's a car in the ego lane
              if (d < (lane_width+lane_width*lane) && d > (lane_width*lane)){
                // std::cout << "There's a vehicle in ego lane" << std::endl;
                // there is a car in the same lane as ego: calculate its velocity
                double vx = sensor_fusion[i][3];
                double vy = sensor_fusion[i][4];
                double check_speed = sqrt(vx*vx + vy*vy);

                // check its position, starting from current s
                double check_car_s = sensor_fusion[i][5];

                // Where will the car be at the end of the path previously planned
                // considering constant velocity and sampling interval
                check_car_s +=((double)prev_size*delta_t*check_speed);

                // Compare the distance between this predicted position and the
                // position of the ego. Compare with a given threshold
                if((check_car_s > car_s) && ((check_car_s - car_s) < 30)){

                  too_close = true;
                }
              }
            }

            // if we're too close slow down
            if (too_close == true){
              std::cout << "SLOWING DOWN TO AVOID COLLISION" << '\n';
              ref_vel -= 0.224;
            }
            else if (ref_vel < 49.5){
              std::cout << "ACCELERATING" << '\n';
              ref_vel+= 0.224;
            }
            else{
              if (init_acc_over == false){
                std::cout << "OVER INIT ACC" << '\n';
                init_acc_over = true;
              }
              std::cout << "MAINTANING SPEED" << '\n';
            }
          }
          
          //====================================================================
          // TRAJ GENERATION
          //====================================================================
          // Initialize  trajectory with previous path]
          //
          for (int i = 0; i < prev_size; ++i) {
            next_x_vals.push_back(previous_path_x[i]);
            next_y_vals.push_back(previous_path_y[i]);
          }

          //====================================================================
          // generate anchor points

          // vectors of sparse points to intepolate using spline
          vector<double> ptsx;
          vector<double> ptsy;

          // reference state for the car
          double ref_x = car_x;
          double ref_y = car_y;
          double ref_yaw = deg2rad(car_yaw);

          // past points
          if (prev_size<2){
              ptsx.push_back(car_x);
              ptsy.push_back(car_y);
          }
          else {
            ref_x = previous_path_x[prev_size - 1];
            ref_y = previous_path_y[prev_size - 1];

            double ref_x_prev = previous_path_x[prev_size - 2];
            double ref_y_prev = previous_path_y[prev_size - 2];
            ref_yaw = atan2(ref_y - ref_y_prev,ref_x - ref_x_prev);

            ptsx.push_back(ref_x_prev);
            ptsx.push_back(ref_x);

            ptsy.push_back(ref_y_prev);
            ptsy.push_back(ref_y);
          }

          // future points
          vector<double> next_wp0 = getXY(car_s + 30,((lane_width/2)+lane_width*lane),map_waypoints_s,map_waypoints_x,map_waypoints_y);
          vector<double> next_wp1 = getXY(car_s + 60,((lane_width/2)+lane_width*lane),map_waypoints_s,map_waypoints_x,map_waypoints_y);
          vector<double> next_wp2 = getXY(car_s + 90,((lane_width/2)+lane_width*lane),map_waypoints_s,map_waypoints_x,map_waypoints_y);

          ptsx.push_back(next_wp0[0]);
          ptsx.push_back(next_wp1[0]);
          ptsx.push_back(next_wp2[0]);

          ptsy.push_back(next_wp0[1]);
          ptsy.push_back(next_wp1[1]);
          ptsy.push_back(next_wp2[1]);

          // shift in car's ref frame
          for(int i = 0; i<ptsx.size(); i++){
            double shift_x = ptsx[i] - ref_x;
            double shift_y = ptsy[i] - ref_y;

            ptsx[i] = (shift_x * cos(0 - ref_yaw) - shift_y * sin(0 - ref_yaw));
            ptsy[i] = (shift_x * sin(0 - ref_yaw) + shift_y * cos(0 - ref_yaw));
          }

          // generate a spline
          tk::spline s;
          s.set_points(ptsx,ptsy);

          //respace according to target vel
          double target_x = 30.0;
          double target_y = s(target_x);
          double target_distance = distance(0,0,target_x,target_y);

          double x_add_on = 0;

          // add them to path
          for (int i = 1; i <= 50 - prev_size; i++){

            double N = target_distance/(delta_t*ref_vel/2.24);
            double x_point = x_add_on + target_x/N;
            double y_point = s(x_point);

            x_add_on = x_point;

            double x_ref = x_point;
            double y_ref = y_point;

            x_point = (x_ref * cos(ref_yaw) - y_ref * sin(ref_yaw));
            y_point = (x_ref * sin(ref_yaw) + y_ref * cos(ref_yaw));

            x_point += ref_x;
            y_point += ref_y;

            next_x_vals.push_back(x_point);
            next_y_vals.push_back(y_point);
          }


          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"control\","+ msgJson.dump()+"]";

          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }  // end "telemetry" if
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }  // end websocket if
  }); // end h.onMessage

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }

  h.run();
}

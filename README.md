# rclcpp extension for the Executor
Most important features listed below:
- Priorities in range [0-98] added to callback and callback groups can be priority enabled
- Spin functions in pre-existing executors (i.e. `SingleThreadedExecutor` and `MultiThreadedExecutor`) upgraded with possibility of picking highest priority callback first from the ready queue 
- Dynamic number of threads for `DynamicMultiThreadedExecutor` class: a dispatcher running at a set priority dispatches the ready callback handles to the spawned consumer threads making them run at a priority proportional to the one of the callback they're handling. New consumers are going to spawned if all are currently busy. Base priority for the consumer and scheduling policies can be selected from the user. Moreover, dispatcher can be easily bound to a different core than the one assigned to consumers.

**NOTE**: modifications are going to be performed **exclusively** to the "foxy" branch

# rclcpp

This repository contains the source code for the ROS Client Library for C++ package, included with a standard install of any ROS 2 distro.

rclcpp provides the standard C++ API for interacting with ROS 2.

## Usage

`#include "rclcpp/rclcpp.hpp"` allows use of the most common elements of the ROS 2 system.

Visit the [rclcpp API documentation](http://docs.ros2.org/foxy/api/rclcpp/) for a complete list of its main components.

### Examples

The ROS 2 tutorials [Writing a simple publisher and subscriber](https://docs.ros.org/en/foxy/Tutorials/Writing-A-Simple-Cpp-Publisher-And-Subscriber.html).
and [Writing a simple service and client](https://docs.ros.org/en/foxy/Tutorials/Writing-A-Simple-Cpp-Service-And-Client.html)
contain some examples of rclcpp APIs in use.

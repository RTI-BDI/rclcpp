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

# rclcpp extension for the Executor made by RTI-BDI team
Most important features listed below:
- Priorities in range [0-98] added to callback and callback groups can be priority enabled
- Spin functions in pre-existing executors (i.e. `SingleThreadedExecutor` and `MultiThreadedExecutor`) upgraded with possibility of picking highest priority callback first from the ready queue 
- Dynamic number of threads for `DynamicMultiThreadedExecutor` class: a dispatcher running at a set priority dispatches the ready callback handles to the spawned consumer threads making them run at a priority proportional to the one of the callback they're handling. New consumers are going to spawned if all are currently busy. Base priority for the consumer and scheduling policies can be selected from the user. Moreover, dispatcher can be easily bound to a different core than the one assigned to consumers.

**NOTE**: modifications are going to be performed **exclusively** to the "foxy" branch, in which you're currently checked in ;-)

## Modifications done to the pre-existing rclcpp Executors and callbacks
In order the extensions made to existing classes are:

- `TimerBase`,`SubscriptionBase`,`ServiceBase`,`ClientBase`and `AnyExecutable` (which is the wrapper for any of the previous four handle) have been all boosted with an unsigned int *priority* value (the value provides setter/getter methods for it, furthermore all constructor and factory methods were modified with an additional and optional value, otherwise automatically sets to its minimum, i.e. `0`);

- `CallbackGroup`: a boolean flag *consider_priority_* was added to notify whether priority will be considered or not within the callback group (getter method `uses_priority()` is provided alongside the optional possibility to specify it in the constructor; default set to `false`);

- `AllocatorMemoryStrategy` got 4 additional implementations to respectively get the next timer/subscription/service/client handle to be executed: the difference from the ones already there is essentially related to the fact that the new ones pick the highest priority in the ready set. We have now alongside `get_next_timer(AnyExecutable&, WeakNodesList&)` the respective `get_next_highest_pr_timer(AnyExecutable&, WeakNodesList&, bool)` to get the highest priority timer handle waiting to be executed in the ready set first (this will be selected just from callback groups flagged to actually consider priority). The same goes for the other types. The last boolean added allows to decide whether to remove from the lists the picked handle or not. The original behaviour of the "non-priority enabled" ones were to remove it regardless of any other consideration, because of the non-caring and completely arbitrary scheduling flow of the rclcpp::Executor (i.e. just pick timers first, then subscription if no timer is available and so on...). With this flag we could add also a further method `get_next_highest_pr_timer(AnyExecutable&, WeakNodesList&)` executable which select the highest priority one among all the 4 subsets of handles (in this case, the final selected one and only that will be removed at the end before returning to the caller);

- `Executor` has received an update wrt. to the methods to get the next executables (i.e. `get_next_ready_executable`, `get_next_executable`), in particular a boolean flag *highest_pr_first* (default value to `false`) marks whether to get the highest priority ready callback first. If none is present (e.g. no ready handles within the priority enabled callback groups), the original executor "scheduling flow" is going to take place. Consequently, `spin_once`, `spin_once_impl` and `spin_some` have been added with the very same flag as well, given the fact that they are the main callers for the previous mentioned methods;

- `SingleThreadedExecutor`: alongside the default`spin()` call, the `spin_highest_pr_first()` has been added to support a scheduling behaviour in which the highest priority executable available from callback groups which are "priority enabled" is selected first for execution (i.e. copy and paste of the original method, where the`get_next_ready_executable` is called with *highest_pr_first* explicitly set to `true`);

- `MultiThreadedExecutor`: alongside the default`spin()` call, the `spin(uint8 priority)` has been added to support the spawning of a executing threads with an explicit, user-set real time priority;

## DynamicMultiThreaded Executor
The most important extension is about the introduction of a new Executor with a smarter and more real-time oriented scheduling and dispatching flow. Two main classes of threads are going to be spawned by it, specifically we have:
- A **Dispatcher** per DynamicMultiThreadedExecutor instance which is going to run typically at a high rt priority value (set by the user) that whenever it finds anything into the readyset, it immediately dispatches it for execution to the first Consumer thread which is free to execute it, setting temporarily its real-time priority to the one of the handle it needs to execute. If no Consumer is available, a new one is going to be spawn (unless a user defined upper-bound has been already reached) and the callback execution will be delegate to it.
- The **Consumer** thread waits for the Dispatcher to wakes him up and, when it does, it executes the assigned callbacks, then put its rt priority value to the base one (set by the user) and goes into waiting mode again. There is the possibility to specificy a maximum waiting time (default behaviour does not consider this), after that the Consumer will automatically terminate itself.
![Alt text](docs/img/Dynamic_MT_rclcpp_executor.drawio.png?raw=true "DynamicMultiThreaded Executor flow chart")

### Constructor methods
``
DynamicMultiThreadedExecutor(
    const rclcpp::ExecutorOptions & options = rclcpp::ExecutorOptions(),
    uint16_t number_of_initial_consumers = 0,
    uint16_t max_number_of_consumers = MAX_CONSUMERS,
    bool yield_between_dispatches = false,
    bool dispatcher_core_bound = false)
``

In addition to the "standard" `ExecutorOptions`, the user can specify the initial and maximum number of consumer threads (default are respectively 1 and 128), whether to call the [`pthread_yield()'](https://man7.org/linux/man-pages/man3/pthread_yield.3.html) between different dispatches and whether to bound the dispatcher run exclusively to a core and the consumers to the remaining one (this obv. won't work on a single core architecture).

### Tunable properties
| Property | Default  | Description  |  Methods |
| :-----:  | :----: | :------------ | :------ |
| `num_starting_consumers_` | `1` | Initial pool of Consumers that is going to be spawned before the Dispatcher cycle begins | `get_number_of_starting_consumers()` `set_number_of_starting_consumers(uint16)` |
| `num_max_consumers_` | `128` | Maximum number of Consumers that is going to be spawned during the `spin()` call | `get_max_number_of_consumers()` `set_max_number_of_consumers(uint16)` |
| `num_active_consumers_` | `0` | Number of active Consumers (returns `0` if called before or after the `spin()` call has terminated); read-only | `get_number_of_active_consumers()` |
| `default_sched_policy_` | `SCHED_FIFO` | Scheduling policy applied for both the Dispatcher and the Consumers at the next `spin()` call | `get_default_sched_policy()` `set_default_sched_policy(int)` |
| `default_dispatcher_pr_` | `51` | Default scheduling real time priority for the Dispatcher enforced at the next `spin()` or `spin(int)` call | `get_default_dispatcher_priority()` `set_default_dispatcher_priority(uint8)` |
| `default_consumer_base_pr_` | `1` | Default scheduling real time priority for the Consumer threads when waiting enforced at the next `spin()` or `spin(int)` call | `get_default_consumer_base_priority()` `set_default_consumer_base_priority(uint8)` |
| `dispatcher_core_bound_` | `false` | Mark whether to execute the Dispatcher exclusively on a (virtual) core (the last one available) different from the ones in which the Consumer threads are going to be executed | `get_dispatcher_core_bound()` `set_dispatcher_core_bound(bool)` |
| `uwait_dispatcher_` | `5` | Timeout in μs that the dispatcher takes when the ready set is empty, waiting for new callbacks to be dispatched | `get_uwait_dispatcher()` `set_uwait_dispatcher(int8)` |
| `max_sec_consumer_wait_` | `INFINITE` | Timeout in s that the consumer takes when it puts itself in waiting for new work. In case no work comes before the times threshold is reached, the Consumer will auto terminate itself (and the dispatcher will need it to respawn it later if the workload rises up again). Note: a negative value means **infinite** | `get_max_consumer_wait()` `set_max_consumer_wait(int8)` |

### Spin functions
The Dispatcher and Consumer threads can be started in multiple ways:

``spin()``

Uses the default values for scheduling policy and scheduling rt priorities.


``spin(int policy)``

Overrides for this spin the default value for scheduling policy, but uses the default ones for scheduling rt priorities.

``spin(int policy, uint8 dispatcher_base_pr, uint8 consumer_base_pr)``

Overrides for this spin the default values for both scheduling policy and for scheduling rt priorities.

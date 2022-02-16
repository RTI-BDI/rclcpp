// Copyright 2014 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef RCLCPP__EXECUTORS__DYNAMIC_MULTI_THREADED_EXECUTOR_HPP_
#define RCLCPP__EXECUTORS__DYNAMIC_MULTI_THREADED_EXECUTOR_HPP_

#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <pthread.h>
#include <limits.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <unordered_map>

#include "rclcpp/detail/mutex_two_priorities.hpp"
#include "rclcpp/executor.hpp"
#include "rclcpp/macros.hpp"
#include "rclcpp/memory_strategies.hpp"
#include "rclcpp/visibility_control.hpp"

namespace rclcpp
{
namespace executors
{

class DynamicMultiThreadedExecutor : public rclcpp::Executor
{

public:
  RCLCPP_SMART_PTR_DEFINITIONS(DynamicMultiThreadedExecutor)

  typedef struct
  {
    DynamicMultiThreadedExecutor* dmt_executor;
    int policy;
    int dispatcher_priority;
    int base_consumer_priority;
  } DispatcherData;

  typedef struct
  {
    DynamicMultiThreadedExecutor* dmt_executor;
    uint16_t my_i;
  } ConsumerData;

  /// Constructor for DynamicMultiThreadedExecutor.
  /**
   * For the yield_before_execute option, when true std::this_thread::yield()
   * will be called after acquiring work (as an AnyExecutable) and
   * releasing the spinning lock, but before executing the work.
   * This is useful for reproducing some bugs related to taking work more than
   * once.
   *
   * \param options common options for all executors
   * \param number_of_initial_consumers number of threads to initially have in the thread pool,
   *   the default 0 will use the number of cpu cores found instead
   * \param yield_between_dispatches if true std::this_thread::yield() is called after a dispatch
   */
  RCLCPP_PUBLIC
  DynamicMultiThreadedExecutor(
    const rclcpp::ExecutorOptions & options = rclcpp::ExecutorOptions(),
    uint16_t number_of_initial_consumers = 0,
    bool yield_between_dispatches = false);

  RCLCPP_PUBLIC
  virtual ~DynamicMultiThreadedExecutor();

  /**
   * \sa rclcpp::Executor:spin() for more details
   * \throws std::runtime_error when spin() called while already spinning
   */
  RCLCPP_PUBLIC
  void
  spin() override;

  RCLCPP_PUBLIC
  void
  spin(const int& policy, const uint8_t& dispatcher_pr, const uint8_t& base_consumer_pr);

  RCLCPP_PUBLIC
  uint16_t
  get_number_of_consumers();

  RCLCPP_PUBLIC
  int
  get_default_sched_policy(){return default_sched_policy_;}

  RCLCPP_PUBLIC
  void
  set_default_sched_policy(const int& policy){
    if(policy == SCHED_OTHER || policy == SCHED_FIFO || policy == SCHED_RR || policy == SCHED_DEADLINE)
      default_sched_policy_ = policy;
  }

  RCLCPP_PUBLIC
  uint8_t
  get_default_dispatcher_priority(){return default_dispatcher_pr_;}

  RCLCPP_PUBLIC
  void
  set_default_dispatcher_priority(const uint8_t& dispatcher_pr){
      default_dispatcher_pr_ = dispatcher_pr;
  }

  RCLCPP_PUBLIC
  uint8_t
  get_default_consumer_base_priority(){return default_consumer_base_pr_;}

  RCLCPP_PUBLIC
  void
  set_default_consumer_base_priority(const uint8_t& consumer_pr){
      default_consumer_base_pr_ = consumer_pr;
  }

  RCLCPP_PUBLIC
  void
  set_uwait_dispatcher(const uint32_t& uwait_dispatcher){
      uwait_dispatcher_ = uwait_dispatcher;
  }

  RCLCPP_PUBLIC
  void
  set_usleep_consumer(const uint32_t& usleep_consumer){
      usleep_consumer_ = usleep_consumer;
  }

protected:

private:
  RCLCPP_DISABLE_COPY(DynamicMultiThreadedExecutor)

  /*
    Utility function to spawn the dispatcher thread, given the pointer to the corresponding DynamicMultiThreadedExecutor
    and the rest of the data necessary for its spawning (note DispatcherData holds the pointer to the object too)
  */
  static int spawn_dispatcher(DispatcherData* dispatcher_data);

  /*
    Wrapper for dispatcher pthread join (accessed though the pointer to the DynamicMultiThreadedExecutor within the instance of DispatcherData)
  */
  static int wait_dispatcher(DispatcherData* dispatcher_data);


  /*
    Utility function to initially spawn number_of_consumers_ with policy, base priority as specified in DispatcherData struct,
    where it's possible to find the reference to the respective DynamicMultiThreadedExecutor object
  */
  static int spawn_new_consumers(DispatcherData* dispatcher_data);

  /*
    Utility function to spawn A NEW consumer with policy, base priority as specified in DispatcherData struct,
    where it's possible to find the reference to the respective DynamicMultiThreadedExecutor object
    number_of_consumers_ will be incremented, as busy and callback and the other relative structures
    All new consumer items will be put on the back of the current vectors
  */
  static int spawn_new_consumer(DispatcherData* dispatcher_data);

  /*
    Function executed by the dispatcher thread
  */
  static void* dispatcher_run(void* data);

  /*
    Function executed by the consumer thread
  */
  static void* consumer_run(void* data);

  /*
    Init pthread, pthread attr and sched params, busy + callbacks vectors
    resize them wrt. current number_of_consumers_ value and initialize every single item to default values
  */
  void init_consumers_data();
  
  // default sched policy for spinning
  int default_sched_policy_;

  // default priority at which runs the dispatcher thread
  uint8_t default_dispatcher_pr_;

  // default base priority at which runs any consumer thread
  uint8_t default_consumer_base_pr_;

  // dispatcher running at the highest priority, repeatedly polling the ReadySet for work to dispatch to consumers (spawning one if needed)
  pthread_t dispatcher_;
  // consumers thread running at the priority given by the callback 
  std::vector<pthread_t> consumers_;

  //sched param and pthread attr for dispatcher
  struct sched_param dispatcher_param_;
  pthread_attr_t dispatcher_attr_;

  //sched param and pthread attr for consumers
  std::vector<struct sched_param> consumers_params_;
  std::vector<pthread_attr_t> consumers_attr_;
  //consumers data
  std::vector<ConsumerData> consumers_data_;

  //flag if a consumer thread is actively executing a callback
  std::vector<std::atomic_bool> busy_;
  //callbacks[i] contains the executable that must be handled by consumer thread i
  std::vector<rclcpp::AnyExecutable> callbacks_;

  // number of consumer threads
  uint16_t number_of_consumers_;

  // yield between dispatches
  bool yield_between_dispatches_;

  // usleep between dispatches
  uint32_t uwait_dispatcher_;
  // usleep between consume
  uint32_t usleep_consumer_;

};

}  // namespace executors
}  // namespace rclcpp

#endif  // RCLCPP__EXECUTORS__DYNAMIC_MULTI_THREADED_EXECUTOR_HPP_

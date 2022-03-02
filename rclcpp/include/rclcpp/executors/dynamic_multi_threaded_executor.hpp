// Copyright 2022 Devis Dal Moro devis.dalmoro@unitn.it, University of Trento
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

#include <pthread.h>
#include <limits.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>

#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <set>

#include "rclcpp/detail/mutex_two_priorities.hpp"
#include "rclcpp/executor.hpp"
#include "rclcpp/macros.hpp"
#include "rclcpp/memory_strategies.hpp"
#include "rclcpp/visibility_control.hpp"

#define MAX_CONSUMERS 128 //default max number of allowable consumers
#define DEFAULT_UWAIT_DISPATCHER 5 //default us timeout for dispatcher, when waiting for next callback to be picked

#define CONS_DEAD 2 // consumer thread is not running
#define CONS_WAIT 0  // consumer thread is running, waiting for new work (NO busy waiting)
#define CONS_BUSY 1  // consumer thread is running and executing a callback

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

  typedef struct{
    int err;
    uint16_t new_cons_i;
  } SpawningResult;

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
   * \param max_number_of_consumers max number of threads within the consumers thread pool
   * \param yield_between_dispatches if true std::this_thread::yield() is called after a dispatch
   * \param dispatcher_core_bound bound always to the same core which will not be assigned to consumers (i.e. they'll have a complementary cpu_set mask)
   */
  RCLCPP_PUBLIC
  DynamicMultiThreadedExecutor(
    const rclcpp::ExecutorOptions & options = rclcpp::ExecutorOptions(),
    uint16_t number_of_initial_consumers = 0,
    uint16_t max_number_of_consumers = MAX_CONSUMERS,
    bool yield_between_dispatches = false,
    bool dispatcher_core_bound = false);

  RCLCPP_PUBLIC
  virtual ~DynamicMultiThreadedExecutor();

  /**
   * \sa rclcpp::Executor:spin() for more details
   * \throws std::runtime_error when spin() called while already spinning
   */
  RCLCPP_PUBLIC
  void
  spin() override
  {
    spin(default_sched_policy_, default_dispatcher_pr_, default_consumer_base_pr_);
  }

  RCLCPP_PUBLIC
  void
  spin(const int& policy)
  {
    spin(policy, default_dispatcher_pr_, default_consumer_base_pr_);
  }

  RCLCPP_PUBLIC
  void
  spin(const int& policy, const uint8_t& dispatcher_base_pr, const uint8_t& base_consumer_pr);

  RCLCPP_PUBLIC
  uint16_t
  get_number_of_starting_consumers(){return num_starting_consumers_;}

  RCLCPP_PUBLIC
  void
  set_number_of_starting_consumers(const uint16_t& starting_consumers){num_starting_consumers_=starting_consumers;}

  RCLCPP_PUBLIC
  uint16_t
  get_max_number_of_consumers(){return num_max_consumers_;}

  RCLCPP_PUBLIC
  void
  set_max_number_of_consumers(const uint16_t& max_consumers){
    num_max_consumers_ = (max_consumers>0)? max_consumers : 1;
  }

  RCLCPP_PUBLIC
  uint16_t
  get_number_of_active_consumers(){return num_active_consumers_;}

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
  void
  set_dispatcher_core_bound(const bool& dispatcher_core_bound){
      dispatcher_core_bound_ = dispatcher_core_bound;
  }

  RCLCPP_PUBLIC
  bool
  get_dispatcher_core_bound(){ return dispatcher_core_bound_; }

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
  uint32_t
  get_uwait_dispatcher(){ return uwait_dispatcher_; }

  RCLCPP_PUBLIC
  void
  set_max_consumer_wait(const int8_t& max_sec_consumer_wait){
      max_sec_consumer_wait_ = max_sec_consumer_wait;
  }

  RCLCPP_PUBLIC
  uint32_t
  get_max_consumer_wait(){ return max_sec_consumer_wait_; }

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
    Wrapper for consumers pthread cancel + join (accessed though the pointer to the DynamicMultiThreadedExecutor within the instance of DispatcherData)
  */
  static int terminate_spawned_consumers(DispatcherData* dispatcher_data);


  /*
    Utility function to initially spawn number_of_consumers_ with policy, base priority as specified in DispatcherData struct,
    where it's possible to find the reference to the respective DynamicMultiThreadedExecutor object
  */
  static int spawn_starting_consumers(DispatcherData* dispatcher_data);

  /*
    Utility function to spawn A NEW consumer with policy, base priority as specified in DispatcherData struct,
    where it's possible to find the reference to the respective DynamicMultiThreadedExecutor object
    number_of_consumers_ will be incremented, as consumers_status_ and callback and the other relative structures
    All new consumer items will be put on the back of the current vectors
  */
  static SpawningResult spawn_new_consumer(DispatcherData* dispatcher_data);

  /*
    Function executed by the dispatcher thread
  */
  static void* dispatcher_run(void* data);

  /*
    Function executed by the consumer thread
  */
  static void* consumer_run(void* data);

  /*
    Init pthread, pthread attr and sched params, consumers_status_ + callbacks vectors
    resize them to max accepted value (at the moment ~128 statically defined) and initialize every single item to default values

    IMPORTANT NOTE: member function: the ones above are all static!!!
  */
  void init_consumers_data(const int& max_consumers);
  
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
  //sched param and pthread attr for consumers
  std::vector<struct sched_param> consumers_params_;
  std::vector<pthread_attr_t> consumers_attr_;
  
  //consumers data
  std::vector<ConsumerData> consumers_data_;
  
  // mutex for consumers' guard lock  
  std::vector<std::mutex> consumers_mutex_;
  // mutex for consumers' cv
  std::vector<std::condition_variable> consumers_cv_;

  //sched param and pthread attr for dispatcher
  struct sched_param dispatcher_param_;
  pthread_attr_t dispatcher_attr_;

  //flag if a consumer thread is actively executing a callback, dead or waiting
  std::vector<std::atomic_int8_t> consumers_status_;
  //callbacks[i] contains the executable that must be handled by consumer thread i
  std::vector<rclcpp::AnyExecutable> callbacks_;

  // number of initial consumer threads
  uint16_t num_starting_consumers_;
  // number of currently active consumer threads
  uint16_t num_active_consumers_;
  // number of maximum allowed consumer threads
  uint16_t num_max_consumers_;

  // max seconds to wait before killing the consumer
  int8_t max_sec_consumer_wait_;

  // yield between dispatches
  bool yield_between_dispatches_;

  // bound dispatcher to a core different than the one of the consumers
  bool dispatcher_core_bound_;

  // usleep between dispatches
  uint32_t uwait_dispatcher_;

};

}  // namespace executors
}  // namespace rclcpp

#endif  // RCLCPP__EXECUTORS__DYNAMIC_MULTI_THREADED_EXECUTOR_HPP_

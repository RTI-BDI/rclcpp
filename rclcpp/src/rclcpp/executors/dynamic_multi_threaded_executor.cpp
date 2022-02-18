// Copyright 2015 Open Source Robotics Foundation, Inc.
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

#include "rclcpp/executors/dynamic_multi_threaded_executor.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "rclcpp/utilities.hpp"
#include "rclcpp/scope_exit.hpp"

#define MAX_CONSUMERS 128
#define DEFAULT_UWAIT_DISPATCHER 5
#define DEFAULT_USLEEP_CONSUMER 5


using rclcpp::executors::DynamicMultiThreadedExecutor;

// //size_t of local variables instantiated within dispatcher_run
// size_t DISPATCHER_STACK_ADD = sizeof(rclcpp::AnyExecutable) + sizeof(int) * 2 + 
//                                 sizeof(std::unique_lock<std::mutex>) + 
//                                 sizeof(DynamicMultiThreadedExecutor*) +
//                                 sizeof(DynamicMultiThreadedExecutor::DispatcherData*);

// //size_t of local variables instantiated within consumer_run
// size_t CONSUMER_STACK_ADD = sizeof(bool) + sizeof(uint16_t) + 
//                                 sizeof(std::unique_lock<std::mutex>) + 
//                                 sizeof(DynamicMultiThreadedExecutor*) +
//                                 sizeof(DynamicMultiThreadedExecutor::ConsumerData*);

static int pthread_setup(struct sched_param *param, pthread_attr_t *attr, const int &policy, const int &priority/*, const size_t &stack_size_add = 0*/)
{
  /* Initialize pthread attributes (default values) */
  int err = pthread_attr_init(attr);
  if (err)
    return err;

  /* Set a specific stack size  */
  // Hey Devis!!! Avoid stack prefaulting for the time being, othwerwise executor's gonna crash
  // err = pthread_attr_setstacksize(attr, PTHREAD_STACK_MIN + stack_size_add);
  // if (err)
  //   return err;

  /* Set scheduler policy and priority of pthread */
  err = pthread_attr_setschedpolicy(attr, policy);
  if (err)
    return err;

  param->sched_priority = priority;
  err = pthread_attr_setschedparam(attr, param);
  if (err)
    return err;

  /* Use scheduling parameters of attr */
  err = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
  if (err)
    return err;

  return err;
}

DynamicMultiThreadedExecutor::DynamicMultiThreadedExecutor(
    const rclcpp::ExecutorOptions &options,
    uint16_t number_of_initial_consumers,
    bool yield_between_dispatches,
    bool dispatcher_core_bound)
    : rclcpp::Executor(options),
      yield_between_dispatches_(yield_between_dispatches),
      dispatcher_core_bound_(dispatcher_core_bound)
{

  uwait_dispatcher_  = DEFAULT_UWAIT_DISPATCHER;
  num_starting_consumers_ = number_of_initial_consumers ? number_of_initial_consumers : static_cast<uint16_t>(sysconf(_SC_NPROCESSORS_ONLN)); // std::thread::hardware_concurrency() specific implementation for linux (https://stackoverflow.com/questions/7341046/posix-equivalent-of-boostthreadhardware-concurrency)
  num_active_consumers_ = 0;
  if (num_starting_consumers_ == 0)
    num_starting_consumers_ = 1;
  else if(num_starting_consumers_ > MAX_CONSUMERS)
    num_starting_consumers_ = MAX_CONSUMERS;
}

DynamicMultiThreadedExecutor::~DynamicMultiThreadedExecutor() {}

/*
  Static utility function to spawn a new dispatcher
*/
int DynamicMultiThreadedExecutor::spawn_dispatcher(DispatcherData* dispatcher_data)
{
  int err = pthread_setup(&(dispatcher_data->dmt_executor->dispatcher_param_), 
                            &(dispatcher_data->dmt_executor->dispatcher_attr_), 
                            dispatcher_data->policy, dispatcher_data->dispatcher_priority/*, DISPATCHER_STACK_ADD*/);

  if (!err)
    err = pthread_create(&(dispatcher_data->dmt_executor->dispatcher_), 
                          &(dispatcher_data->dmt_executor->dispatcher_attr_), 
                          DynamicMultiThreadedExecutor::dispatcher_run, dispatcher_data);

  if(!err && dispatcher_data->dmt_executor->dispatcher_core_bound_)
  {
    u_int8_t num_cores = static_cast<uint8_t>(sysconf(_SC_NPROCESSORS_ONLN));
    if(num_cores > 1)
    {
      cpu_set_t t_cpu_set;
      //bound disptacher to a single core, while consumer will be placed on the others
      CPU_ZERO(&t_cpu_set);
      CPU_SET(num_cores-1, &t_cpu_set);//set dispatcher to last core available (consumers will be set to all others)
      pthread_setaffinity_np(dispatcher_data->dmt_executor->dispatcher_, sizeof(t_cpu_set),
                          &t_cpu_set);
    }
  }

  return err;
}

/*
  Static utility function: wrapper for dispatcher join
*/
int DynamicMultiThreadedExecutor::wait_dispatcher(DispatcherData* dispatcher_data)
{
  return pthread_join(dispatcher_data->dmt_executor->dispatcher_, NULL);
}

/*
  Static utility function: wrapper for cancel+join all active consumers
*/
int DynamicMultiThreadedExecutor::terminate_spawned_consumers(DispatcherData* dispatcher_data)
{
  int err = 0;
  printf("Disabling all %d active consumers threads\n", dispatcher_data->dmt_executor->num_active_consumers_);
  //sending cancel signal to all active consumer threads
  for(uint16_t i = 0; i<dispatcher_data->dmt_executor->num_active_consumers_; i++)
    err += pthread_cancel(dispatcher_data->dmt_executor->consumers_[i]);

  //wating for them to parse and respond to the cancel
  for(uint16_t i = 0; i<dispatcher_data->dmt_executor->num_active_consumers_; i++)
    err += pthread_join(dispatcher_data->dmt_executor->consumers_[i], NULL);

  return err;
}

/*
  Utility function to initially spawn num_starting_consumers_ with policy, base priority as specified in DispatcherData struct,
  where it's possible to find the reference to the respective DynamicMultiThreadedExecutor object
*/
int DynamicMultiThreadedExecutor::spawn_starting_consumers(DispatcherData* dispatcher_data)
{
  DynamicMultiThreadedExecutor* dmt_exec = ((DynamicMultiThreadedExecutor*) dispatcher_data->dmt_executor);
  int err = 0;
  for (int i = 0; !err && i < dmt_exec->num_starting_consumers_; i++)
    err += spawn_new_consumer(dispatcher_data);

  return err;
}

/*
  Utility function to spawn A NEW consumer with policy, base priority as specified in DispatcherData struct,
  where it's possible to find the reference to the respective DynamicMultiThreadedExecutor object
  num_active_consumers_ will be incremented, as busy and callback and the other relative structures
  All new consumer items will be put on the back of the current vectors
*/
int DynamicMultiThreadedExecutor::spawn_new_consumer(DispatcherData* dispatcher_data)
{
  DynamicMultiThreadedExecutor* dmt_exec = dispatcher_data->dmt_executor;
  if(dmt_exec->num_active_consumers_ >= MAX_CONSUMERS)//no othre consumer can be spawned
    return 1;

  int err = 0;//return value (zero -> success, otherwise error code)
  uint16_t new_cons_i = dmt_exec->num_active_consumers_;//new consumer index
  dmt_exec->num_active_consumers_++;

  dmt_exec->busy_[new_cons_i].store(false);// new consumer is NOT busy
  err += pthread_setup(&(dmt_exec->consumers_params_[new_cons_i]), &(dmt_exec->consumers_attr_[new_cons_i]), 
                          dispatcher_data->policy, dispatcher_data->base_consumer_priority/*, CONSUMER_STACK_ADD*/);
  

  if(!err)
    err += pthread_create(&(dmt_exec->consumers_[new_cons_i]), &(dmt_exec->consumers_attr_[new_cons_i]), 
                            DynamicMultiThreadedExecutor::consumer_run, &(dmt_exec->consumers_data_[new_cons_i]));
  
  if(!err && dmt_exec->dispatcher_core_bound_)
  {
    uint8_t num_cores = static_cast<uint8_t>(sysconf(_SC_NPROCESSORS_ONLN));
    if(num_cores > 1)
    {
      cpu_set_t t_cpu_set;
      //bound consumers to all cores, except last one reserved for the dispatcher
      CPU_ZERO(&t_cpu_set);
      for(uint8_t i = 0; i<num_cores-2; i++)
        CPU_SET(i, &t_cpu_set);
      pthread_setaffinity_np(dmt_exec->consumers_[new_cons_i], sizeof(t_cpu_set),
                          &t_cpu_set);
    }
  }

  if(err)//creation failed -> rollback number of active consumers
    dmt_exec->num_active_consumers_--;

  return err;
}

/*
  Returns index of the first false boolean within the busy array
*/
static int get_free_consumer(const std::vector<std::atomic_bool>& busy, const uint16_t& number_of_consumers)
{
  for(u_int16_t i=0; i < number_of_consumers; i++)
    if(!busy[i].load())
      return i;//free consumer
  return -1;
}

void *DynamicMultiThreadedExecutor::consumer_run(void *data)
{
  ConsumerData* cons_data = (ConsumerData *)data;
  uint16_t my_i = cons_data->my_i;
  bool kill_myself = false;
  DynamicMultiThreadedExecutor* dmt_exec = cons_data->dmt_executor;
  while (!kill_myself)
  {
    std::unique_lock<std::mutex> lck(dmt_exec->consumers_mutex_[my_i]);
    dmt_exec->consumers_cv_[my_i].wait(lck, [&]() {return dmt_exec->busy_[my_i].load();});//block iff !busy
    if(dmt_exec->busy_[my_i].load())
    {
      //work to do
      dmt_exec->execute_any_executable(dmt_exec->callbacks_[my_i]);
      dmt_exec->callbacks_[my_i] = AnyExecutable{};//callback executed
      pthread_setschedprio(dmt_exec->consumers_[my_i], dmt_exec->default_consumer_base_pr_);//reset my priority to default base pr for consumers
      dmt_exec->busy_[my_i].store(false);//not busy anymore
    }
  }
  return NULL;
}

void *DynamicMultiThreadedExecutor::dispatcher_run(void *data)
{
  DispatcherData* d_data = (DispatcherData *)data;
  DynamicMultiThreadedExecutor* dmt_exec = d_data->dmt_executor;
  int err = spawn_starting_consumers(d_data);

  if (!err)
  {
    while (rclcpp::ok(dmt_exec->context_) && dmt_exec->spinning.load()) {
      rclcpp::AnyExecutable any_executable;

      if (dmt_exec->get_next_executable(any_executable, std::chrono::nanoseconds(dmt_exec->uwait_dispatcher_ * 1000), false)) {//wait for uwait_dispatcher_ for work to become available if none, before new polling
        int free_cons_i = get_free_consumer(dmt_exec->busy_, dmt_exec->num_active_consumers_);
        if(free_cons_i == -1)
        {
          //no free consumer, spawn new consumer thread
          err = spawn_new_consumer(d_data);
          if(err)//error during spawning
            continue;

          free_cons_i = dmt_exec->num_active_consumers_ - 1;
        }
        
        dmt_exec->callbacks_[free_cons_i] = any_executable;//assign callback to selected consumer
        pthread_setschedprio(dmt_exec->consumers_[free_cons_i], any_executable.priority);//assign to the consumer thread its new priority
        dmt_exec->busy_[free_cons_i].store(true);// mark the consumer as busy

        std::unique_lock<std::mutex> lck(dmt_exec->consumers_mutex_[free_cons_i]);//acquire lock for signaling the waiting consumers
        dmt_exec->consumers_cv_[free_cons_i].notify_one();//notify passive waiting consumer

        if(dmt_exec->yield_between_dispatches_)
          pthread_yield();//force rescheduling

        //any_executable.callback_group.reset();
      }
    }
  }

  terminate_spawned_consumers(d_data);

  return NULL;
}

/*
  Init pthread, pthread attr and sched params, busy + callbacks vectors
  resize them to max accepted value (at the moment ~128 statically defined) and initialize every single item to default values
*/
void DynamicMultiThreadedExecutor::init_consumers_data()
{
  // For now avoid resizes later by putting each array to the maximum number of consumers
  // since the pointers to the single cell shall be passed to consumers, we want to avoid
  // that resizes with copy and paste of the original sized array (might be triggered by push_back too)
  // in a new area causes faulty access leading to brutal crashes
  consumers_ = std::vector<pthread_t>(MAX_CONSUMERS);
  consumers_params_ = std::vector<struct sched_param>(MAX_CONSUMERS);
  consumers_attr_ = std::vector<pthread_attr_t>(MAX_CONSUMERS);
  consumers_mutex_ = std::vector<std::mutex>(MAX_CONSUMERS);
  consumers_cv_ = std::vector<std::condition_variable>(MAX_CONSUMERS);
  busy_ = std::vector<std::atomic_bool>(MAX_CONSUMERS);
  for (u_int16_t i = 0; i < MAX_CONSUMERS; i++)
  {
    consumers_data_.push_back(ConsumerData{this, i});
    busy_[i].store(false);
  }
  callbacks_ = std::vector<AnyExecutable>(MAX_CONSUMERS);
}

void DynamicMultiThreadedExecutor::spin(const int &policy, const uint8_t &dispatcher_pr, const uint8_t &base_consumer_pr)
{

  if (spinning.exchange(true))
  {
    throw std::runtime_error("spin() called while already spinning");
  }
  RCLCPP_SCOPE_EXIT(this->spinning.store(false););

  // init data for consumer threads
  init_consumers_data();// resize pthread, pthread attr and sched params, busy + callbacks vectors
  // init data for dispatcher thread and put it into the respective structure
  DispatcherData dispatcher_data = DispatcherData{this, policy, dispatcher_pr, base_consumer_pr};

  int err = 0;

  /* Lock current and future memory to prevent page faults before spawning any new thread */
  if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
  {
    throw std::runtime_error("mlockall failed: %m failed during spin\n");
    exit(-2);
  }

  err = spawn_dispatcher(&dispatcher_data);
  if(err)
  {
    throw std::runtime_error("dispatcher spawn failed\n");
    exit(-2);
  }

  err = wait_dispatcher(&dispatcher_data);
  if (err)
  {
    throw std::runtime_error("dispatcher thread joined failed\n");
    exit(-2);
  }
}
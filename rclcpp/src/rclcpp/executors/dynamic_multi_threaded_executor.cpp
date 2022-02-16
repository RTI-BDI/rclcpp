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

#include <stdlib.h> //TODO(Devis) remove it later, just for rand tests
#include <time.h> //TODO(Devis) remove it later, just for rand tests

#include "rclcpp/utilities.hpp"
#include "rclcpp/scope_exit.hpp"

#define MAX_CONSUMERS 128
#define DEFAULT_UWAIT_DISPATCHER 5
#define DEFAULT_USLEEP_CONSUMER 5

using rclcpp::executors::DynamicMultiThreadedExecutor;

static int pthread_setup(struct sched_param *param, pthread_attr_t *attr, const int &policy, const int &priority/*, const size_t &stack_size_add = 0*/)
{
  /* Initialize pthread attributes (default values) */
  int ret = pthread_attr_init(attr);
  if (ret)
  {
    //printf("init pthread attributes failed\n");
    return ret;
  }

  /* Set a specific stack size  */
  // Hey Devis!!! Avoid stack prefaulting for the time being, othwerwise executor's gonna crash
  // ret = pthread_attr_setstacksize(attr, PTHREAD_STACK_MIN + stack_size_add);
  // if (ret)
  // {
  //   //printf("pthread setstacksize failed\n");
  //   return ret;
  // }

  /* Set scheduler policy and priority of pthread */
  ret = pthread_attr_setschedpolicy(attr, policy);
  if (ret)
  {
    //printf("pthread setschedpolicy failed\n");
    return ret;
  }

  param->sched_priority = priority;
  ret = pthread_attr_setschedparam(attr, param);
  if (ret)
  {
    //printf("pthread setschedparam failed\n");
    return ret;
  }

  /* Use scheduling parameters of attr */
  ret = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
  if (ret)
  {
    //printf("pthread setinheritsched failed\n");
    return ret;
  }

  return ret;
}

DynamicMultiThreadedExecutor::DynamicMultiThreadedExecutor(
    const rclcpp::ExecutorOptions &options,
    uint16_t number_of_initial_consumers,
    bool yield_between_dispatches)
    : rclcpp::Executor(options),
      yield_between_dispatches_(yield_between_dispatches)
{
  uwait_dispatcher_  = DEFAULT_UWAIT_DISPATCHER;
  usleep_consumer_ = DEFAULT_USLEEP_CONSUMER;//
  number_of_consumers_ = number_of_initial_consumers ? number_of_initial_consumers : uint16_t(sysconf(_SC_NPROCESSORS_ONLN)); // std::thread::hardware_concurrency() specific implementation for linux (https://stackoverflow.com/questions/7341046/posix-equivalent-of-boostthreadhardware-concurrency)
  if (number_of_consumers_ == 0)
    number_of_consumers_ = 1;
  else if(number_of_consumers_ > MAX_CONSUMERS)
    number_of_consumers_ = MAX_CONSUMERS;
}

DynamicMultiThreadedExecutor::~DynamicMultiThreadedExecutor() {}

/*
  Static utility function to spawn a new dispatcher
*/
int DynamicMultiThreadedExecutor::spawn_dispatcher(DispatcherData* dispatcher_data)
{
  int ret = pthread_setup(&(dispatcher_data->dmt_executor->dispatcher_param_), 
                            &(dispatcher_data->dmt_executor->dispatcher_attr_), 
                            dispatcher_data->policy, dispatcher_data->dispatcher_priority
                          /*, sizeof(rclcpp::AnyExecutable) + sizeof(int) * (number_of_consumers_ + 1)*/);
  if (!ret)
    ret = pthread_create(&(dispatcher_data->dmt_executor->dispatcher_), 
                          &(dispatcher_data->dmt_executor->dispatcher_attr_), 
                          DynamicMultiThreadedExecutor::dispatcher_run, dispatcher_data);

  return ret;
}

/*
  Static utility function: wrapper for dispatcher join
*/
int DynamicMultiThreadedExecutor::wait_dispatcher(DispatcherData* dispatcher_data)
{
  return pthread_join(dispatcher_data->dmt_executor->dispatcher_, NULL);
}

/*
  Utility function to initially spawn number_of_consumers_ with policy, base priority as specified in DispatcherData struct,
  where it's possible to find the reference to the respective DynamicMultiThreadedExecutor object
*/
int DynamicMultiThreadedExecutor::spawn_new_consumers(DispatcherData* dispatcher_data)
{
  DynamicMultiThreadedExecutor* dmt_exec = dispatcher_data->dmt_executor;
  int ret = 0;
  for (int i = 0; i < dmt_exec->number_of_consumers_; i++)
    ret += pthread_setup(&(dmt_exec->consumers_params_[i]), &(dmt_exec->consumers_attr_[i]), 
                          dispatcher_data->policy, dispatcher_data->base_consumer_priority/*, sizeof(int)*/);

  if (!ret)
  {
    for (uint16_t i = 0; i < dmt_exec->number_of_consumers_; i++)
    {
      ret += pthread_create(&(dmt_exec->consumers_[i]), &(dmt_exec->consumers_attr_[i]), 
              DynamicMultiThreadedExecutor::consumer_run, &(dmt_exec->consumers_data_[i]));
    }
  }
  return ret;
}

/*
  Utility function to spawn A NEW consumer with policy, base priority as specified in DispatcherData struct,
  where it's possible to find the reference to the respective DynamicMultiThreadedExecutor object
  number_of_consumers_ will be incremented, as busy and callback and the other relative structures
  All new consumer items will be put on the back of the current vectors
*/
int DynamicMultiThreadedExecutor::spawn_new_consumer(DispatcherData* dispatcher_data)
{
  DynamicMultiThreadedExecutor* dmt_exec = dispatcher_data->dmt_executor;
  if(dmt_exec->number_of_consumers_ >= MAX_CONSUMERS)//no othre consumer can be spawned
    return 1;
  int err = 0;//return value (zero -> success, otherwise error code)

  uint16_t new_cons_i = dmt_exec->number_of_consumers_;//new consumer index
  dmt_exec->number_of_consumers_++;
  
  pthread_t new_cons_thread;
  struct sched_param new_cons_param;
  pthread_attr_t new_cons_attr;
  dmt_exec->consumers_.push_back(new_cons_thread);
  dmt_exec->consumers_params_.push_back(new_cons_param);
  dmt_exec->consumers_attr_.push_back(new_cons_attr);
  dmt_exec->consumers_data_.push_back(ConsumerData{dmt_exec, new_cons_i});
  // Devis you don't need to push new elem to busy_ -> sized to MAX_CONSUMERS at the start
  dmt_exec->busy_[new_cons_i].store(false);
  dmt_exec->callbacks_.push_back(AnyExecutable{});

  ConsumerData cons_data{dmt_exec, new_cons_i};

  err += pthread_setup(&(dmt_exec->consumers_params_[new_cons_i]), &(dmt_exec->consumers_attr_[new_cons_i]), 
                          dispatcher_data->policy, dispatcher_data->base_consumer_priority/*, sizeof(int)*/);

  if(!err)
    err += pthread_create(&(dmt_exec->consumers_[new_cons_i]), &(dmt_exec->consumers_attr_[new_cons_i]), 
                            DynamicMultiThreadedExecutor::consumer_run, &cons_data);
  if(err)
  {
    //clean up pushed elements and set back number of consumer to original
    dmt_exec->number_of_consumers_--;
    dmt_exec->consumers_.pop_back();
    dmt_exec->consumers_params_.pop_back();
    dmt_exec->consumers_attr_.pop_back();
    dmt_exec->consumers_data_.pop_back();
    dmt_exec->callbacks_.pop_back();
  }

  return err;
}

/*
  Returns index of the first false boolean within the busy array
*/
static int get_free_consumer(const std::vector<std::atomic_bool>& busy)
{
  for(u_int16_t i=0; i < (static_cast<uint16_t> (busy.size())); i++)
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
  //printf("Consumer %d of DMT rclcpp Executor started\n", my_i);
  while (!kill_myself)
  {
    if(dmt_exec->busy_[my_i].load())
    {
      //work to do
      //printf("Consumer %d ready to execute a callback of type %s \n", my_i, (dmt_exec->callbacks_[my_i].timer)? "timer" : (dmt_exec->callbacks_[my_i].subscription)? "subscription" : "other");
      dmt_exec->execute_any_executable(dmt_exec->callbacks_[my_i]);
      dmt_exec->callbacks_[my_i] = AnyExecutable{};//callback executed
      pthread_setschedprio(dmt_exec->consumers_[my_i], dmt_exec->default_consumer_base_pr_);//reset my priority to default base pr for consumers
      if(dmt_exec->usleep_consumer_)
        usleep(dmt_exec->usleep_consumer_);
      dmt_exec->busy_[my_i].store(false);//not busy anymore
    }
  }
  return NULL;
}

void *DynamicMultiThreadedExecutor::dispatcher_run(void *data)
{
  ////printf("Dispatcher of DMT rclcpp Executor started\n");
  DispatcherData *d_data = (DispatcherData *)data;
  DynamicMultiThreadedExecutor* dmt_exec = d_data->dmt_executor;
  int err = spawn_new_consumers(d_data);

  if (!err)
  {
    while (rclcpp::ok(dmt_exec->context_) && dmt_exec->spinning.load()) {
      rclcpp::AnyExecutable any_executable;

      if (dmt_exec->get_next_executable(any_executable, std::chrono::nanoseconds(dmt_exec->uwait_dispatcher_), false)) {//wait for 5us for work to become available if none, before new polling
        //printf("Dispatching a callback of type=%s\n", (any_executable.timer)? "timer" : (any_executable.subscription)? "subscription" : "other");
        int free_cons_i = get_free_consumer(dmt_exec->busy_);
        if(free_cons_i == -1)
        {
          //printf("No free consumer found for executing a callback of type=%s\n", (any_executable.timer)? "timer" : (any_executable.subscription)? "subscription" : "other");
          //no free consumer, spawn new consumer thread
          err = spawn_new_consumer(d_data);
          if(err)//error during spawning
            continue;
          free_cons_i = dmt_exec->number_of_consumers_ - 1;
        }
        //printf("Dispatching callback of type=%s to consumer %d\n", (any_executable.timer)? "timer" : (any_executable.subscription)? "subscription" : "other", free_cons_i);
        dmt_exec->callbacks_[free_cons_i] = any_executable;//assign callback to selected consumer
        pthread_setschedprio(dmt_exec->consumers_[free_cons_i], any_executable.priority);//assign to the consumer thread its new priority
        dmt_exec->busy_[free_cons_i].store(true);// mark the consumer as busy

        if(dmt_exec->yield_between_dispatches_)
          pthread_yield();//force rescheduling

        any_executable.callback_group.reset();
      }
    }
  }

  return NULL;
}

/*
  Init pthread, pthread attr and sched params, busy + callbacks vectors
  resize them wrt. current number_of_consumers_ value and initialize every single item to default values
*/
void DynamicMultiThreadedExecutor::init_consumers_data()
{
  consumers_ = std::vector<pthread_t>(number_of_consumers_);
  consumers_params_ = std::vector<struct sched_param>(number_of_consumers_);
  consumers_attr_ = std::vector<pthread_attr_t>(number_of_consumers_);
  busy_ = std::vector<std::atomic_bool>(MAX_CONSUMERS); //impossible to resize later
  for (u_int16_t i = 0; i < MAX_CONSUMERS; i++)
  {
    if(i < number_of_consumers_)
      consumers_data_.push_back(ConsumerData{this, i});
    busy_[i].store(false);
  }
  callbacks_ = std::vector<AnyExecutable>(number_of_consumers_);
}

void DynamicMultiThreadedExecutor::spin()
{
  spin(default_sched_policy_, default_dispatcher_pr_, default_consumer_base_pr_);
}

void DynamicMultiThreadedExecutor::spin(const int &policy, const uint8_t &dispatcher_pr, const uint8_t &base_consumer_pr)
{
  srand( static_cast<unsigned int>(time(NULL)) );

  if (spinning.exchange(true))
  {
    throw std::runtime_error("spin() called while already spinning");
  }
  RCLCPP_SCOPE_EXIT(this->spinning.store(false););

  // init data for consumer threads
  init_consumers_data();// resize pthread, pthread attr and sched params, busy + callbacks vectors
  // init data for dispatcher thread and put it into the respective structure
  DispatcherData dispatcher_data = DispatcherData{this, policy, dispatcher_pr, base_consumer_pr};

  int ret = 0;

  /* Lock current and future memory to prevent page faults before spawning any new thread */
  if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
  {
    throw std::runtime_error("mlockall failed: %m failed during spin\n");
    exit(-2);
  }

  ret = spawn_dispatcher(&dispatcher_data);
  if(ret)
  {
    throw std::runtime_error("dispatcher spawn failed\n");
    exit(-2);
  }

  ret = wait_dispatcher(&dispatcher_data);
  if (ret)
  {
    throw std::runtime_error("dispatcher thread joined failed\n");
    exit(-2);
  }
}

uint16_t
DynamicMultiThreadedExecutor::get_number_of_consumers()
{
  return number_of_consumers_;
}

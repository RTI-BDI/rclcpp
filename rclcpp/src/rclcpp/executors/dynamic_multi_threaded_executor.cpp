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

#include "rclcpp/executors/dynamic_multi_threaded_executor.hpp"

#include <time.h>
#include <chrono>
#include <functional>

#include "rclcpp/utilities.hpp"
#include "rclcpp/scope_exit.hpp"

using rclcpp::executors::DynamicMultiThreadedExecutor;

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
    uint16_t max_number_of_consumers,
    bool yield_between_dispatches,
    bool dispatcher_core_bound)
    : rclcpp::Executor(options),
      yield_between_dispatches_(yield_between_dispatches),
      dispatcher_core_bound_(dispatcher_core_bound)
{

  uwait_dispatcher_  = DEFAULT_UWAIT_DISPATCHER;
  num_starting_consumers_ = number_of_initial_consumers ? number_of_initial_consumers : static_cast<uint16_t>(sysconf(_SC_NPROCESSORS_ONLN)); // std::thread::hardware_concurrency() specific implementation for linux (https://stackoverflow.com/questions/7341046/posix-equivalent-of-boostthreadhardware-concurrency)
  num_active_consumers_ = 0;
  max_sec_consumer_wait_ = -1;//default behaviour does not let consumers go destroy themselves
  default_sched_policy_ = DEFAULT_SCHED_POLICY;
  default_consumer_base_pr_ = DEFAULT_SCHED_RT_PR_CONSUMER;
  default_dispatcher_pr_ = DEFAULT_SCHED_RT_PR_DISPATCHER;

  num_max_consumers_ = (max_number_of_consumers>0)? max_number_of_consumers : 1;

  if (num_starting_consumers_ == 0)
    num_starting_consumers_ = 1;
  else if(num_starting_consumers_ > num_max_consumers_)
    num_starting_consumers_ = num_max_consumers_;
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
  for(uint16_t i = 0; i<dispatcher_data->dmt_executor->num_max_consumers_; i++)
    if(dispatcher_data->dmt_executor->consumers_status_[i].load() != CONS_DEAD)
      err += pthread_cancel(dispatcher_data->dmt_executor->consumers_[i]);

  //wating for them to parse and respond to the cancel
  for(uint16_t i = 0; i<dispatcher_data->dmt_executor->num_max_consumers_; i++)
    if(dispatcher_data->dmt_executor->consumers_status_[i].load() != CONS_DEAD)
      err += pthread_join(dispatcher_data->dmt_executor->consumers_[i], NULL);

  return err;
}


char debug_str[1024];

/*
  Returns index of the first consumer with a certain status value within the consumers_status array
*/
static int get_first_consumer_by_status(const std::vector<std::atomic_int8_t>& consumers_status, 
                                        const uint16_t& number_of_consumers,
                                        const int8_t& CONS_STATUS)
{
  for(u_int16_t i=0; i < number_of_consumers; i++)
  {
    int status = consumers_status[i].load();
    debug_str[i*2] = (char) ('0' + status);
    debug_str[i*2+1] = ',';
    if(status == CONS_STATUS)
      return i;//free consumer
  }
  printf("%s\n", debug_str);
  return -1;
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
    err += spawn_new_consumer(dispatcher_data).err;

  return err;
}

/*
  Utility function to spawn A NEW consumer with policy, base priority as specified in DispatcherData struct,
  where it's possible to find the reference to the respective DynamicMultiThreadedExecutor object
  num_active_consumers_ will be incremented, as consumers_status_ and callback and the other relative structures
  All new consumer items will be put on the back of the current vectors
*/
DynamicMultiThreadedExecutor::SpawningResult DynamicMultiThreadedExecutor::spawn_new_consumer(DispatcherData* dispatcher_data)
{
  auto res = SpawningResult{1, 0};
  DynamicMultiThreadedExecutor* dmt_exec = dispatcher_data->dmt_executor;
  printf("Spawning new consumer (active consumers = %d, max consumers = %d)\n", dmt_exec->num_active_consumers_, dmt_exec->num_max_consumers_);
  if(dmt_exec->num_active_consumers_ >= dmt_exec->num_max_consumers_)//no other consumer can be spawned
    return res;

  res.err = 0;//return value (zero -> success, otherwise error code)
  int new_cons_i = get_first_consumer_by_status(dmt_exec->consumers_status_, dmt_exec->num_max_consumers_, CONS_DEAD);//new consumer index
  printf("Consumer %d ready to be spawned\n", new_cons_i);
  dmt_exec->num_active_consumers_++;
  
  if(dmt_exec->max_sec_consumer_wait_ > 0)
    pthread_join(dispatcher_data->dmt_executor->consumers_[new_cons_i], NULL);//thread might have killed itself just an instant before
  dmt_exec->consumers_status_[new_cons_i].store(CONS_WAIT);// new consumer is running and NOT busy
  res.err += pthread_setup(&(dmt_exec->consumers_params_[new_cons_i]), &(dmt_exec->consumers_attr_[new_cons_i]), 
                          dispatcher_data->policy, dispatcher_data->base_consumer_priority/*, CONSUMER_STACK_ADD*/);
  
  printf("Consumer %d ready to be spawned: setup done err=%d \n", new_cons_i, res.err);
  if(!res.err)
    res.err += pthread_create(&(dmt_exec->consumers_[new_cons_i]), &(dmt_exec->consumers_attr_[new_cons_i]), 
                            DynamicMultiThreadedExecutor::consumer_run, &(dmt_exec->consumers_data_[new_cons_i]));
  
  printf("Consumer %d  spawned: create done err=%d \n", new_cons_i, res.err);
  if(!res.err && dmt_exec->dispatcher_core_bound_)
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

  if(res.err){
    //creation failed -> rollback number of active consumers and selected consumer status
    dmt_exec->num_active_consumers_--;  
    dmt_exec->consumers_status_[new_cons_i].store(CONS_DEAD);
  }

  return res;
}

void *DynamicMultiThreadedExecutor::consumer_run(void *data)
{
  ConsumerData* cons_data = (ConsumerData *)data;
  uint16_t my_i = cons_data->my_i;
  printf("Consumer %d spawned!\n", my_i);
  bool kill_myself = false;
  timespec start_wait, end_wait;
  DynamicMultiThreadedExecutor* dmt_exec = cons_data->dmt_executor;
  while (!kill_myself)
  { 
    std::unique_lock<std::mutex> lck(dmt_exec->consumers_mutex_[my_i]);
    
    if(dmt_exec->consumers_status_[my_i].load() != CONS_BUSY)
    {
      if(dmt_exec->max_sec_consumer_wait_ > 0)//register start of waiting
        clock_gettime(CLOCK_MONOTONIC, &start_wait);

      dmt_exec->consumers_cv_[my_i].wait_for(lck, std::chrono::seconds(dmt_exec->max_sec_consumer_wait_));//block iff !busy
    }

    if(dmt_exec->consumers_status_[my_i].load() == CONS_BUSY)
    {
      //work to do
      dmt_exec->execute_any_executable(dmt_exec->callbacks_[my_i]);
      dmt_exec->callbacks_[my_i] = AnyExecutable{};//callback executed
      pthread_setschedprio(dmt_exec->consumers_[my_i], dmt_exec->default_consumer_base_pr_);//reset my priority to default base pr for consumers
      dmt_exec->consumers_status_[my_i].store(CONS_WAIT);//not busy anymore
    
    }
    else if(dmt_exec->max_sec_consumer_wait_ > 0)
    {
      clock_gettime(CLOCK_MONOTONIC, &end_wait);
      
      if( dmt_exec->max_sec_consumer_wait_ > (end_wait.tv_sec-start_wait.tv_sec)
           ||
          (dmt_exec->max_sec_consumer_wait_ == (end_wait.tv_sec-start_wait.tv_sec) && end_wait.tv_nsec >= start_wait.tv_nsec)
      )
      {
        if(dmt_exec->consumers_status_[my_i].exchange(CONS_DEAD) == CONS_BUSY)
          dmt_exec->consumers_status_[my_i].store(CONS_BUSY);//I became busy just instant before killing my self... pheeeew
        else
          kill_myself = true;//surpassed time limit of waiting in idle
      }
    }
  }
  return NULL;
}

void *DynamicMultiThreadedExecutor::dispatcher_run(void *data)
{
  DispatcherData* d_data = (DispatcherData *)data;
  DynamicMultiThreadedExecutor* dmt_exec = d_data->dmt_executor;
  int err = spawn_starting_consumers(d_data);
  SpawningResult spawning_res;

  if (!err)
  {
    while (rclcpp::ok(dmt_exec->context_) && dmt_exec->spinning.load()) {
      rclcpp::AnyExecutable any_executable;

      if (dmt_exec->get_next_executable(any_executable, std::chrono::nanoseconds(dmt_exec->uwait_dispatcher_ * 1000), false)) {//wait for uwait_dispatcher_ for work to become available if none, before new polling
        
        int free_cons_i = get_first_consumer_by_status(dmt_exec->consumers_status_, dmt_exec->num_max_consumers_, CONS_WAIT);
        printf("Getting free consumer %d to execute\n", free_cons_i);
        if(free_cons_i == -1)
        {
          //no free consumer, spawn new consumer thread
          spawning_res = spawn_new_consumer(d_data);
          if(spawning_res.err)//error during spawning
            continue;

          free_cons_i = spawning_res.new_cons_i;
          printf("Spawned new consumer %d to execute\n", free_cons_i);
        }
        
        dmt_exec->callbacks_[free_cons_i] = any_executable;//assign callback to selected consumer
        pthread_setschedprio(dmt_exec->consumers_[free_cons_i], any_executable.priority);//assign to the consumer thread its new priority
        dmt_exec->consumers_status_[free_cons_i].store(CONS_BUSY);// mark the consumer as busy

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
  Init pthread, pthread attr and sched params, consumers_status_ + callbacks vectors
  resize them to max accepted value (at the moment ~128 statically defined) and initialize every single item to default values
*/
void DynamicMultiThreadedExecutor::init_consumers_data(const int& max_consumers)
{
  // For now avoid resizes later by putting each array to the maximum number of consumers
  // since the pointers to the single cell shall be passed to consumers, we want to avoid
  // that resizes with copy and paste of the original sized array (might be triggered by push_back too)
  // in a new area causes faulty access leading to brutal crashes
  consumers_ = std::vector<pthread_t>(max_consumers);
  consumers_params_ = std::vector<struct sched_param>(max_consumers);
  consumers_attr_ = std::vector<pthread_attr_t>(max_consumers);
  consumers_mutex_ = std::vector<std::mutex>(max_consumers);
  consumers_cv_ = std::vector<std::condition_variable>(max_consumers);
  consumers_status_ = std::vector<std::atomic_int8_t>(max_consumers);
  for (u_int16_t i = 0; i < max_consumers; i++)
  {
    consumers_data_.push_back(ConsumerData{this, i});
    consumers_status_[i].store(CONS_DEAD);//consumer status initially put to dead (i.e. not running)
  }
  callbacks_ = std::vector<AnyExecutable>(max_consumers);
}

void DynamicMultiThreadedExecutor::spin(const int &policy, const uint8_t &dispatcher_base_pr, const uint8_t &base_consumer_pr)
{

  if (spinning.exchange(true))
  {
    throw std::runtime_error("spin() called while already spinning");
  }
  RCLCPP_SCOPE_EXIT(this->spinning.store(false););

  // init data for consumer threads
  init_consumers_data(num_max_consumers_);// resize pthread, pthread attr and sched params, busy + callbacks vectors
  // init data for dispatcher thread and put it into the respective structure
  DispatcherData dispatcher_data = DispatcherData{this, policy, dispatcher_base_pr, base_consumer_pr};

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
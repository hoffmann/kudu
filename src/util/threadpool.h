// Copyright (c) 2013, Cloudera, inc.
// All rights reserved.
#ifndef KUDU_UTIL_THREAD_POOL_H
#define KUDU_UTIL_THREAD_POOL_H

#include <boost/function.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <tr1/memory>
#include <list>
#include <string>
#include <vector>

#include "gutil/gscoped_ptr.h"
#include "gutil/macros.h"
#include "gutil/port.h"
#include "util/monotime.h"
#include "util/status.h"

namespace kudu {

class ThreadPool;
class Trace;

class Runnable {
 public:
  virtual void Run() = 0;
  virtual ~Runnable() {}
};

// ThreadPool takes a lot of arguments. We provide sane defaults with a builder.
//
// name: Used for debugging output and default names of the worker threads.
//    Since thread names are limited to 16 characters on Linux, it's good to
//    choose a short name here.
//    Required.
//
// min_threads: Minimum number of threads we'll have at any time.
//    Default: 0.
//
// max_threads: Maximum number of threads we'll have at any time.
//    Default: Number of CPUs detected on the system.
//
// max_queue_size: Maximum number of items to enqueue before returning a
//    Status::ServiceUnavailable message from Submit().
//    Default: INT_MAX.
//
// timeout: How long we'll keep around an idle thread before timing it out.
//    We always keep at least min_threads.
//    Default: 500 milliseconds.
//
class ThreadPoolBuilder {
 public:
  explicit ThreadPoolBuilder(const std::string& name);

  // Note: We violate the style guide by returning mutable references here
  // in order to provide traditional Builder pattern conveniences.
  ThreadPoolBuilder& set_min_threads(int min_threads);
  ThreadPoolBuilder& set_max_threads(int max_threads);
  ThreadPoolBuilder& set_max_queue_size(int max_queue_size);
  ThreadPoolBuilder& set_idle_timeout(const MonoDelta& idle_timeout);

  const std::string& name() const { return name_; }
  int min_threads() const { return min_threads_; }
  int max_threads() const { return max_threads_; }
  int max_queue_size() const { return max_queue_size_; }
  const MonoDelta& idle_timeout() const { return idle_timeout_; }

  // Instantiate a new ThreadPool with the existing builder arguments.
  Status Build(gscoped_ptr<ThreadPool>* pool) const;

 private:
  friend class ThreadPool;
  const std::string name_;
  int min_threads_;
  int max_threads_;
  int max_queue_size_;
  MonoDelta idle_timeout_;

  DISALLOW_COPY_AND_ASSIGN(ThreadPoolBuilder);
};

// Thread pool with a variable number of threads.
// The pool can execute a class that implements the Runnable interface, or a
// boost::function, which can be obtained via boost::bind().
//
// Usage Example:
//    static void Func(int n) { ... }
//    class Task : public Runnable { ... }
//
//    gscoped_ptr<ThreadPool> thread_pool;
//    CHECK_OK(
//        ThreadPoolBuilder("my_pool")
//            .set_min_threads(0)
//            .set_max_threads(5)
//            .set_max_queue_size(10)
//            .set_timeout(MonoDelta::FromMilliseconds(2000))
//            .Build(&thread_pool));
//    thread_pool->Submit(shared_ptr<Runnable>(new Task()));
//    thread_pool->Submit(boost::bind(&Func, 10));
class ThreadPool {
 public:
  ~ThreadPool();

  // Wait for the running tasks to complete and then shutdown the threads.
  // All the other pending tasks in the queue will be removed.
  // NOTE: That the user may implement an external abort logic for the
  //       runnables, that must be called before Shutdown(), if the system
  //       should know about the non-execution of these tasks, or the runnable
  //       require an explicit "abort" notification to exit from the run loop.
  void Shutdown();

  // Submit a function binded using boost::bind(&FuncName, args...)
  Status SubmitFunc(const boost::function<void()>& func)
      WARN_UNUSED_RESULT;

  // Submit a Runnable class
  Status Submit(const std::tr1::shared_ptr<Runnable>& task)
      WARN_UNUSED_RESULT;

  // Wait until all the tasks are completed.
  void Wait();

  // Waits for the idle state for the given duration of time.
  // Returns true if the pool is idle within the given timeout. Otherwise false.
  //
  // For example:
  //  thread_pool.TimedWait(boost::posix_time::milliseconds(100));
  template<class TimeDuration>
  bool TimedWait(const TimeDuration& relative_time) {
    return TimedWait(boost::get_system_time() + relative_time);
  }

  // Waits for the idle state for the given duration of time.
  // Returns true if the pool is idle within the given timeout. Otherwise false.
  bool TimedWait(const boost::system_time& time_until);

 private:
  friend class ThreadPoolBuilder;

  // Create a new thread pool using a builder.
  explicit ThreadPool(const ThreadPoolBuilder& builder);

  // Initialize the thread pool by starting the minimum number of threads.
  Status Init();

  // Clear all entries from queue_. Requires that lock_ is held.
  void ClearQueue();

  // Dispatcher responsible for dequeueing and executing the tasks
  void DispatchThread(bool permanent);

  // Create new thread. Required that lock_ is held.
  Status CreateThreadUnlocked();

 private:
  FRIEND_TEST(TestThreadPool, TestThreadPoolWithNoMinimum);
  FRIEND_TEST(TestThreadPool, TestVariableSizeThreadPool);

  struct QueueEntry {
    std::tr1::shared_ptr<Runnable> runnable;
    Trace* trace;
  };

  const std::string name_;
  const int min_threads_;
  const int max_threads_;
  const int max_queue_size_;
  const MonoDelta idle_timeout_;

  Status pool_status_;
  boost::condition_variable idle_cond_;
  boost::condition_variable no_threads_cond_;
  boost::condition_variable not_empty_;
  int num_threads_;
  int active_threads_;
  int queue_size_;
  boost::mutex lock_;
  std::list<QueueEntry> queue_;

  DISALLOW_COPY_AND_ASSIGN(ThreadPool);
};

} // namespace kudu
#endif

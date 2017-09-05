#ifndef _QUITTABLE_SLEEPER
#define _QUITTABLE_SLEEPER 1

// A class that assists with fast shutdown of threads. You can set
// a flag that says the thread should quit, which it can then check
// in a loop -- and if the thread sleeps (using the sleep_* functions
// on the class), that sleep will immediately be aborted.
//
// All member functions on this class are thread-safe.

#include <chrono>
#include <mutex>

class QuittableSleeper {
public:
	void quit()
	{
		std::lock_guard<std::mutex> l(mu);
		should_quit_var = true;
		quit_cond.notify_all();
	}

	void unquit()
	{
		std::lock_guard<std::mutex> l(mu);
		should_quit_var = false;
	}

	void wakeup()
	{
		std::lock_guard<std::mutex> l(mu);
		should_wakeup_var = true;
		quit_cond.notify_all();
	}

	bool should_quit() const
	{
		std::lock_guard<std::mutex> l(mu);
		return should_quit_var;
	}

	// Returns false if woken up early.
	template<class Rep, class Period>
	bool sleep_for(const std::chrono::duration<Rep, Period> &duration)
	{
		std::chrono::steady_clock::time_point t =
			std::chrono::steady_clock::now() +
			std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);
		return sleep_until(t);
	}

	// Returns false if woken up early.
	template<class Clock, class Duration>
	bool sleep_until(const std::chrono::time_point<Clock, Duration> &t)
	{
		std::unique_lock<std::mutex> lock(mu);
		quit_cond.wait_until(lock, t, [this]{
			return should_quit_var || should_wakeup_var;
		});
		if (should_wakeup_var) {
			should_wakeup_var = false;
			return false;
		}
		return !should_quit_var;
	}

private:
	mutable std::mutex mu;
	bool should_quit_var = false, should_wakeup_var = false;
	std::condition_variable quit_cond;
};

#endif  // !defined(_QUITTABLE_SLEEPER) 

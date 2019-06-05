#include "../include/threadpool.h"
#include <unordered_set>

using namespace tcl;
using namespace std;

struct CvtStringToInt {
	inline size_t operator ()(const std::string& from) const { return std::stoul(from); }
};

static inline void set_thread_name(const std::string&& name)
{
	pthread_setname_np(pthread_self(), name.c_str());
}

static inline void set_thread_affinity(size_t cpu_id) {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu_id, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

threadpool::threadpool(size_t InitialWorkers, size_t MaxWorkers, const std::vector<size_t>& CoreBinding, const std::vector<size_t>& CoreExclude)
	: PoolYet(true), PoolInitWorkers(InitialWorkers), PoolMaxWorkers(MaxWorkers), PoolIndexWorkers(0), PoolBusyWorkers(0)
{

	/* Initialize CPUs bindings  */
	{
		size_t max_threads = std::thread::hardware_concurrency();
		unordered_set<size_t> uniqCores,excludeCores(CoreExclude.begin(), CoreExclude.end());

		if (CoreBinding.empty()) {
			for (; max_threads--;) {
				if (excludeCores.find(max_threads) == excludeCores.end()) {
					uniqCores.emplace(max_threads);
				}
			}
		}
		else {
			for (auto&& co : CoreBinding) {
				if (excludeCores.find(co % max_threads) == excludeCores.end()) {
					uniqCores.emplace(co % max_threads);
				}
			}
		}
		for (auto&& co : uniqCores) {
			PoolWorkersBinding.emplace_back(co);
		}
	}
	if (PoolWorkersBinding.empty() || !PoolInitWorkers || PoolMaxWorkers < PoolInitWorkers) {
		throw std::runtime_error("Invalid thread pool initialization.");
	}

	hire(PoolInitWorkers, sPersistent);
}

threadpool::~threadpool() {
	join();
}

void threadpool::join() {
	if (PoolYet) {
		{
			unique_lock<mutex> lock(PoolQueueSync);
			PoolYet = false;
		}
		PoolCondition.notify_all();
		for (auto& worker : PoolWorkers) {
			if (worker.second.joinable()) {
				worker.second.join();
			}
		}
	}
}

void threadpool::stats(size_t& NumWorkers, size_t& NumAwaitingTasks, size_t& NumBusy) {
	{
		unique_lock<mutex> lock(PoolQueueSync);
		NumWorkers = PoolWorkers.size();
		NumAwaitingTasks = PoolTasks.size();
	}
	NumBusy = (long)PoolBusyWorkers;
}

void threadpool::check_resize(size_t NumWorker, size_t NumTasks) {
	auto AvailableWorkers = NumWorker - (long)PoolBusyWorkers;
	size_t NewWorkerCount = 0;

	if (NumTasks > AvailableWorkers) {
		NewWorkerCount = size_t(((NumTasks - AvailableWorkers) + (PoolInitWorkers - 1)) / PoolInitWorkers) * PoolInitWorkers;
	}

	if ((NumWorker + NewWorkerCount) > PoolMaxWorkers) {
		NewWorkerCount = PoolMaxWorkers - NumWorker;
	}

	if (NewWorkerCount > 0) {
		hire(NewWorkerCount, sSecondary);
	}
}

void threadpool::hire(size_t NumWorkers, threadpool::WorkerType Type) {
	unique_lock<mutex> lock(PoolQueueSync);
	while (NumWorkers--)
	{
		std::thread ant([](
			size_t core, size_t num, atomic_bool& Yet, mutex& Sync, condition_variable& Condition,
			queue< std::function<void()> >& Tasks, threads& Workers, atomic_long& Busy, threadpool::WorkerType Type)
			{
				string thread_name(string(Type == sPersistent ? "Peristent" : (Type == sSecondary ? "Secondary" : (Type == sAsync ? "Async" : "Unknown"))) + "#" + to_string(num));
				auto timeout = std::chrono::milliseconds(10000);

				set_thread_name(move(thread_name));
				set_thread_affinity(core);

				while (1) {
					function<void()> task;
					{
						unique_lock<mutex> lock(Sync);
						Condition.wait_for(lock, timeout, [&Yet, &Tasks]
							{
								return !Yet || !Tasks.empty();
							});


						/* false negative */
						if (Tasks.empty()) {
							if (!Yet) { break; }
							if (Type == sPersistent) continue;
							auto&& th = Workers.find(this_thread::get_id());
							if (th != Workers.end()) {
								th->second.detach();
								//dbg_trace("Leave thread: `%s` (%ld)", thread_name.c_str(), Workers.erase(this_thread::get_id()));
							}
							else {
								//dbg_trace("Gone thread: `%s` (%ld)", thread_name.c_str(), this_thread::get_id());
							}
							break;
						}

						task = std::move(Tasks.front());
						Tasks.pop();
					}
					Busy += 1;
					task();
					Busy -= 1;
				}
			}, PoolWorkersBinding[PoolIndexWorkers % PoolWorkersBinding.size()], PoolIndexWorkers, ref(PoolYet), ref(PoolQueueSync), ref(PoolCondition), ref(PoolTasks), ref(PoolWorkers), ref(PoolBusyWorkers), Type);
		PoolIndexWorkers++;
		PoolWorkers.emplace(ant.get_id(), move(ant));
	}
}
#pragma once
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <future>


namespace tcl {
	class threadpool {
	private:
		using threads = std::unordered_map< std::thread::id, std::thread >;
		enum WorkerType { sPersistent, sSecondary, sAsync };
		std::atomic_bool				PoolYet;
		size_t							PoolInitWorkers;
		size_t							PoolMaxWorkers;
		size_t							PoolIndexWorkers;
		std::atomic_long				PoolBusyWorkers;
		threads							PoolWorkers;
		std::queue< std::function<void()> >	PoolTasks;
		std::mutex						PoolQueueSync;
		std::condition_variable			PoolCondition;
		std::vector<size_t>				PoolWorkersBinding;
		inline void check_resize(size_t NumWorker, size_t NumTasks) {
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
		inline void hire(size_t NumWorkers, WorkerType Type) {
			std::unique_lock<std::mutex> lock(PoolQueueSync);
			while (NumWorkers--)
			{
				std::thread ant([](
					size_t core, size_t num, std::atomic_bool& Yet, std::mutex& Sync, std::condition_variable& Condition,
					std::queue< std::function<void()> >& Tasks, threads& Workers, std::atomic_long& Busy, threadpool::WorkerType Type)
					{
						std::string thread_name(std::string(Type == sPersistent ? "Peristent" : (Type == sSecondary ? "Secondary" : (Type == sAsync ? "Async" : "Unknown"))) + "#" + to_string(num));
						auto timeout = std::chrono::milliseconds(10000);

						set_thread_name(move(thread_name));
						set_thread_affinity(core);

						while (1) {
							std::function<void()> task;
							{
								std::unique_lock<std::mutex> lock(Sync);
								Condition.wait_for(lock, timeout, [&Yet, &Tasks]
									{
										return !Yet || !Tasks.empty();
									});


								/* false negative */
								if (Tasks.empty()) {
									if (!Yet) { break; }
									if (Type == sPersistent) continue;
									auto&& th = Workers.find(std::this_thread::get_id());
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

		template<typename F, typename Tuple, size_t ...S >
		static inline decltype(auto)  apply_tuple_impl(F&& fn, Tuple&& t, std::index_sequence<S...>)
		{
			return fn(std::get<S>(std::forward<Tuple>(t))...);
		}
		template<typename F, typename Tuple>
		static inline decltype(auto) apply_from_tuple(F&& fn, Tuple&& t)
		{
			std::size_t constexpr tSize
				= std::tuple_size<typename std::remove_reference<Tuple>::type>::value;
			return apply_tuple_impl(std::forward<F>(fn),
				std::forward<Tuple>(t),
				std::make_index_sequence<tSize>());
		}
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
	public:
		/*
		* InitialWorkers - number of persistent workers
		* MaxWorkers - number of maximum workers (auto scale up to)
		* CoreBinding - binding threads to cores list
		* CoreExclude - exclude core from binding
		*/
		threadpool(size_t InitialWorkers, size_t MaxWorkers, const std::vector<size_t>& CoreBinding = {}, const std::vector<size_t>& CoreExclude = {}) : 
			PoolYet(true), PoolInitWorkers(InitialWorkers), PoolMaxWorkers(MaxWorkers), PoolIndexWorkers(0), PoolBusyWorkers(0)
		{

			/* Initialize CPUs bindings  */
			{
				size_t max_threads = std::thread::hardware_concurrency();
				std::unordered_set<size_t> uniqCores, excludeCores(CoreExclude.begin(), CoreExclude.end());

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
		~threadpool() {
			join();
		}

		/*
		* Core binding list
		*/
		inline const std::vector<size_t>& cores() const { return PoolWorkersBinding; }

		/*
		* Wait when all task is processed and all threads down. 
		* After call join() no any task will be added
		*/
		inline void join() {
			if (PoolYet) {
				{
					std::unique_lock<std::mutex> lock(PoolQueueSync);
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


		/*
		* Get current pool status
		*/
		inline void stats(size_t& NumWorkers, size_t& NumAwaitingTasks, size_t& NumBusy) {
			{
				std::unique_lock<std::mutex> lock(PoolQueueSync);
				NumWorkers = PoolWorkers.size();
				NumAwaitingTasks = PoolTasks.size();
			}
			NumBusy = (long)PoolBusyWorkers;
		}

		/*
		* Enqueue job with class handler
		*/
		template<class CLASS, class... Args>
		void enqueue(Args&& ... args) {

			auto args_list = std::tuple<Args...>(args...);

			size_t numWorkers = 0, numTasks = 0;
			{
				std::unique_lock<std::mutex> lock(PoolQueueSync);
				if (!PoolYet)
					throw std::runtime_error("enqueue on stopped ThreadPool");

				PoolTasks.emplace([args_list]() { CLASS obj; apply_from_tuple(obj, args_list); });

				numWorkers = PoolWorkers.size();
				numTasks = PoolTasks.size();
			}
			check_resize(numWorkers, numTasks);
			PoolCondition.notify_one();
		}

		/*
		* Enqueue job with lambda or function handler
		*/
		template<class FN, class... Args>
		auto enqueue(FN&& f, Args&& ... args) -> std::future<typename std::result_of<FN(Args...)>::type> {

			using return_type = typename std::result_of<FN(Args...)>::type;

			auto task = std::make_shared< std::packaged_task<return_type()> >(
				std::bind(std::forward<FN>(f), std::forward<Args>(args)...)
				);

			size_t numWorkers = 0, numTasks = 0;
			std::future<return_type> res = task->get_future();
			{
				std::unique_lock<std::mutex> lock(PoolQueueSync);
				if (!PoolYet)
					throw std::runtime_error("enqueue on stopped ThreadPool");

				PoolTasks.emplace([task]() { (*task)(); });

				numWorkers = PoolWorkers.size();
				numTasks = PoolTasks.size();
			}
			check_resize(numWorkers, numTasks);
			PoolCondition.notify_one();
			return res;
		}
	};
}
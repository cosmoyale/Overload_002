#include <memory>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <tuple>
#include <memory>
#include <algorithm>
#include <set>
#include <pthread.h>

#include <boost/lexical_cast.hpp>
#include <boost/lockfree/queue.hpp>

#include "getcc.h"
#include "bad_queue.hpp"

template <int Align>
int simpleTest(const std::string& pc);

// TODO better namespace name
namespace Thread
{
std::atomic<bool> g_pstart(false);
std::atomic<bool> g_cstart(false);

std::set<std::string> g_output;

std::unique_ptr<uint64_t>
	GetStore ( uint64_t iter )
{
	thread_local uint64_t* store{nullptr};

	if (store == nullptr)
		store = new uint64_t[iter];

	return std::unique_ptr<uint64_t>(store);
}

std::unique_ptr<uint64_t> 
	GetTravelStore ( uint64_t iter )
{
	thread_local uint64_t* store{nullptr};

	if (store == nullptr)
		store = new uint64_t[iter];

	return std::unique_ptr<uint64_t>(store);
}

std::mutex g_cout_lock;

}
struct Benchmark
{
	uint64_t cycles{0};
	uint32_t serial{0};
};

template <typename Bench, int X>
struct Alignment
{
	alignas(X) Bench cb;
	Bench& get() { return cb; }
};

template <typename T>
void genStats (   uint32_t iterations
				, T& data
				, const std::string& tag
				, std::ostream& os)
{
	uint64_t min = 9999999999999;
	uint64_t max = 0;
	double avg = 0;
	double sdev = 0;

	auto cycle_hist = data.get();
	std::sort(cycle_hist, cycle_hist+iterations);
	for (uint32_t i = 0; i < iterations; ++i)
	{
		min = std::min(cycle_hist[i], min);
		max = std::max(cycle_hist[i], max);
		avg += cycle_hist[i];
	}

	avg /= iterations;

	for (uint32_t i = 0; i < iterations; ++i)
	{
		double d =
			static_cast<double>(cycle_hist[i]) 
			- avg;

		sdev += d*d;
	}

	sdev /= iterations;
	double std_dev = sqrt(sdev);

	int p90th = iterations *0.9;
	int p99th = iterations *0.99;
	int p992th = iterations *0.992;
	int p994th = iterations *0.994;
	int p998th = iterations *0.998;
	int p999th = iterations *0.999;

	os	<< tag << ": "
		<< iterations
		<< " avg cyc = " << avg 
		<< ", min = " << min
		<< ", max = " << max 
		<< ", avg = " << avg
		<< ", stddev = " << std_dev

		<< ", 90th = " << cycle_hist[p90th]
		<< ", 99th = " << cycle_hist[p99th]
		<< ", 99.2th = " << cycle_hist[p992th]
		<< ", 99.4th = " << cycle_hist[p994th]
		<< ", 99.8th = " << cycle_hist[p998th]
		<< ", 99.9th = " << cycle_hist[p999th]
		<< ", max = " << 
			cycle_hist[iterations-1];
}

template <typename T, typename Q>
void producer(Q* q, uint32_t iterations)
{
	auto store = Thread::GetStore(iterations);

	while (Thread::g_pstart.load() == false) {}

	T d;

	d.get().serial = 0;

	int result = 0;
	bool work = false;

	for ( uint32_t j = 0; j < 2; ++j) // warm up
	for ( uint32_t i = 0; i < iterations; ++i)
	{
		++d.get().serial;
		do 
		{ 
			d.get().cycles = getcc_ns();
			work = (q->push(d));
			//if(!work)
			//	__builtin_ia32_pause();
		} while (!work); 
		
		
		store.get()[i] = 
			getcc_ns() - d.get().cycles;

		// busy work to throttle production 
		// to eliminiate "stuffed" queue
		/* No noticable effect
		for (uint32_t k = 0; k<1000; ++k)
		{
			result += k+i;
		}
		// */
	}
	++result;

	std::stringstream push;
	genStats(iterations, store, "1 Push", push);

	std::lock_guard<std::mutex> 
		lock(Thread::g_cout_lock);
	
	std::cout << result << std::endl;
	Thread::g_output.emplace(push.str());
}

template <typename T, typename Q>
void consumer(Q* q, uint32_t iterations)
{
	auto store = Thread::GetStore(iterations);
	auto travel_store = 
		Thread::GetTravelStore(iterations);

	while (Thread::g_cstart.load() == false) {}

	T d;
	uint64_t start;
	uint64_t end;
	bool work = false;

	for ( uint32_t j = 0; j < 2; ++j) // warm up
	for ( uint32_t i = 0; i < iterations; ++i)
	{
		do 
		{ 
			start = getcc_ns();
			work = q->pop(d);
			//if (!work)
			//	__builtin_ia32_pause();
		} while (!work);

		end = getcc_ns();

		travel_store.get()[i] = 
				end - d.get().cycles;
		
		store.get()[i] = end - start;
	}

	std::stringstream trvl, pop;
	
	genStats(	iterations, 
				travel_store, 
				"3 Travel", 
				trvl);

	genStats(	iterations, 
				store, 
				"2 Pop", 
				pop);

	std::lock_guard<std::mutex> 
		lock(Thread::g_cout_lock);
	
	Thread::g_output.emplace(pop.str());
	Thread::g_output.emplace(trvl.str());
}

void setAffinity(	
		  std::unique_ptr<std::thread>& t 
		, uint32_t cpuid )
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpuid, &cpuset);

    int rc = pthread_setaffinity_np(
			t->native_handle()
			, sizeof(cpu_set_t)
			, &cpuset);

	std::cerr	<< "affinity " 
				<< cpuid 
				<< std::endl;

	if (rc != 0) 
	{
		std::cerr << "Error calling "
					 "pthread_setaffinity_np: "
				  << rc 
				  << "\n";
		exit (0);
	}
}

template<typename T,template<class...>typename Q>
void run ( const std::string& pc )
{
	std::cout	<< "Alignment of T " 
				<< alignof(T) 
				<< std::endl;

	std::vector<std::unique_ptr<std::thread>> 
		threads;
	
	threads.reserve(pc.length());

	Q<T> q(8192);

	// need to make this a command line option 
	// and do proper balancing between 
	// consumers and producers
	uint32_t iterations = 10000000;

    uint32_t core{0};
    for (auto i : pc)
    {
        if (i == 'p')
        {
            threads.push_back(
                    std::make_unique<std::thread>
                    (producer<T,Q<T>>
                     , &q 
                     , iterations));
            setAffinity(*threads.rbegin(), core);
        }
        else if (i == 'c')
        {
            threads.push_back(
                    std::make_unique<std::thread>		  
                    (consumer<T,Q<T>>
                     , &q
                     , iterations));

            // adjust for physical cpu/core layout
            setAffinity(*threads.rbegin(), core);
        }
        ++core;
    }

	Thread::g_cstart.store(true);
	usleep(500000);
	Thread::g_pstart.store(true);

	for (auto& i : threads)
	{
		i->join();
	}

	for (auto& i : Thread::g_output)
	{
		std::cout << i << std::endl;
	}
}

int main ( int argc, char* argv[] )
{
	if (argc < 3)
	{
		std::cout	<< "Usage: " 
					<< argv[0] 
					<< " <cl|nocl> <producers> "
					"<consumers>" 
					<< std::endl
					<< "Or '" << argv[0] << " Simple'"
					<< std::endl;
		return 0;
	}

    std::string pc{argv[2]};

    uint32_t core{0};
    for (auto i : pc)
    {
        if (i == 'p')
            std::cout << core << ":P ";
        else if (i == 'c')
            std::cout << core << ":C ";
        else
            std::cout << core << ":N ";

        ++core;
    }

    std::cout << std::endl;

	std::cout << "Compiler chosen Alignment of "
				 "Benchmark is " 
			  << alignof(Benchmark) 
			  << std::endl;


	std::string cl(argv[1]);
		
	if (cl == "cl")
	{
		run<Alignment<
			  Benchmark, 64>
			, boost::lockfree::queue> 
                (pc);
	}
	else if (cl == "nocl")
	{
		run<Alignment<
			  Benchmark 
			, alignof(Benchmark)>
			, boost::lockfree::bad_queue>
                (pc);
	}
	else if (cl == "SimpleCL")
	{
		simpleTest<64>(pc);
	}
	else if (cl == "SimpleNOCL")
	{
		simpleTest<4>(pc);
	}

	else
	{
		std::cout 
			<< "First argument must be 'cl'"
			"or 'nocl'" 
			<< std::endl;
		return 0;
	}

	return 0;
}


template <int X>
struct DataTest
{
	alignas (X) std::atomic<uint32_t> d{0};
};

void CLTest ( std::atomic<uint32_t>& d )
{
	for (;;)
	{
		++d;
	}
}

template <int Align>
int simpleTest (const std::string& pc)
{
	// Not sure what header file these are locaated, they are not in #include <new>
	//std::cout << "std::hardware_destructive_interference_size = " << std::hardware_destructive_interference_size << std::endl;
	//std::cout << "std::hardware_constructive_interference_size = " << std::hardware_constructive_interference_size << std::endl;
	
	// change 64 to 4 or 8 to see the degadated performance

	using DataType_t = DataTest<Align>;
	DataType_t data[128];

    std::cout << "Sizeof data = " << sizeof(data) << std::endl;

	std::vector<std::unique_ptr<std::thread>> 
		threads;
	
	threads.reserve(pc.length());

    int32_t core{0};
    int32_t idx{0};
    for(auto i : pc)
    {
        if (i == 'p')
        {
            threads.push_back(std::make_unique<std::thread>(CLTest, std::ref(data[idx].d)));
            setAffinity(*threads.rbegin(), core);
            ++idx;
        }
        ++core;
    }
    
    auto counters = std::make_unique<uint32_t[]>(idx);

	for (;;)
	{
		sleep(1);

        int32_t i{0};
        counters[i] = data[i].d.load(std::memory_order_acquire);
		data[i].d.store(0, std::memory_order_relaxed);
        for (i = 1; i < idx-1; ++i)
        {
            counters[i] = data[i].d.load(std::memory_order_relaxed);
            data[i].d.store(0, std::memory_order_relaxed);
        }
        counters[i] = data[i].d.load(std::memory_order_relaxed);
		data[i].d.store(0, std::memory_order_release);


        uint64_t total{0};
        for (int i = 0; i < idx; ++i)
        {
            std::cout << "d" << i << " = " << counters[i] << ", ";
            total+= counters[i];
        }
        std::cout << ", total = " << total << ", avg = " << static_cast<float>(total) / idx << std::endl;
        total = 0;
	}

	return 0;
}

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

constexpr uint32_t g_precision = 10000;
constexpr float     g_CPUGHzSpeed = 3.0;

// TODO better namespace name
namespace Thread
{
std::atomic<bool> g_pstart(false);
std::atomic<bool> g_cstart(false);

}
struct Benchmark
{
    uint32_t workIterations{0};
    uint32_t workCycles{0};
};

template <typename Bench, int X>
struct Alignment
{
	alignas(X) Bench cb;
	Bench& get() { return cb; }
};

// 1/2 cache line
struct ReadWorkData
{
    enum Size : uint32_t { Elem = 8 };
    std::atomic<uint32_t> data[Elem];
};

// 1/2 cache line
struct WriteWorkData
{
    enum Size : uint32_t { Elem = 8 };
    std::atomic<uint32_t> data[Elem];
};

template <int X>
struct WorkData
{
    WorkData() 
    { 
        for (uint32_t i=0; i<ReadWorkData::Elem; ++i)
            rwd.data[i].store(0);

        for (uint32_t i=0; i<WriteWorkData::Elem; ++i)
            wwd.data[i].store(0);
    }

    WorkData(const WorkData& wd)
    {
        if (&wd == this)
            return;

        for (uint32_t i=0; i<ReadWorkData::Elem; ++i)
            rwd.data[i].store(wd.rwd.data[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    };

    WorkData& operator=(const WorkData& wd)
    {
        if (&wd == this)
            return *this;

        for (uint32_t i=0; i<ReadWorkData::Elem; ++i)
            wwd.data[i].store(wd.wwd.data[i].load(std::memory_order_relaxed), std::memory_order_relaxed);

        return *this;
    }

    alignas (X) ReadWorkData rwd;
    alignas (X) WriteWorkData wwd;
};

///////////////////////////////////////////////////////////////////////////////
// Duty cycle, saturation, testing
///////////////////////////////////////////////////////////////////////////////
struct Results
{
    Results() {}
    uint32_t bandwidth_{0}; // return raw messages per result?
    uint16_t saturationCycles_{0};
    uint16_t saturationRatio_{0};
    
	// using this always returns 0
	//float saturationRatio_{0};

	
	auto saturationCycles() { return static_cast<float>(saturationCycles_)/g_precision; }
	auto saturationRatio() { return static_cast<float>(saturationRatio_)/g_precision; }
	auto bandwidth() { return bandwidth_; }
};

union ResultsSync
{
    alignas(sizeof(uint64_t)) Results results_;
    std::atomic<uint64_t> atomicCopy_;

    ResultsSync() : atomicCopy_(0) {}

    ResultsSync& operator=(ResultsSync& rs)
    {
        if (&rs == this)
            return *this;

        std::atomic<uint64_t>& ac = atomicCopy_;
        std::atomic<uint64_t>& rs_ac = rs.atomicCopy_;

        ac.store(rs_ac.load(std::memory_order_acquire), std::memory_order_release);

        return *this;
    }
};

struct CycleTracker
{
    enum ControlFlags : uint32_t
    {
          Clear     = 1
        , Readable  = 2
    };

    uint64_t start_{0};
    uint64_t end_{0};
    uint64_t overhead_{0};
    uint64_t saturation_{0};
    uint32_t polls_{0};
    uint32_t works_{0};

    // There is intentional false sharing on this, however the impact is unmasurable
    // as long as getResults is called infrequently
    // for the purpose of monitoring we will update this once a second
    std::atomic<uint32_t> controlFlags_{0};

    CycleTracker() {}

    void start()
    {
        start_ = getcc_ns();
    }

    void end()
    {
        end_ = getcc_ns();
    }

    void calcResults(ResultsSync& rs)
    {
        end();
        rs.results_.saturationCycles_ = saturationCycles();
        rs.results_.saturationRatio_ = saturationRatio();
        rs.results_.bandwidth_ = bandwidth(1'000'000);//(end_ - start_) * works_;
    }

    Results getResults(ResultsSync& rs, bool reset = true)
    {
        ResultsSync r;

        // false sharing other thread
        if (cleared())
        {
            return r.results_;
        }

        r = rs;
        if (reset)
            setClear();
        return r.results_;
    }

    // one billion is once per second
    // one millino is once per millisecond
    // one thousand is once per microsecond
    // works_ is the number of work unites executed this observation period.
    // T1: Begin
    uint32_t bandwidth(uint32_t per = 1'000'000'000)
    {
        // 3 is CPU speed in GHz (needs to be set per host)
        // per is observation timescale units
        // end_ - start_ is the observation window.
        return (static_cast<float>(works_) / ((end_ - start_)/(g_CPUGHzSpeed*per)));//*g_precision;
    }
    // T1: End

    // T3: Begin
    uint16_t saturationCycles()
    {
        if (saturation_)
            return static_cast<uint16_t>(   (static_cast<float>(saturation_) / (saturation_ + overhead_))* g_precision    );
        else
            return 0;
    }
    // T3: Begin

    // T2: Begin
    uint16_t saturationRatio()
    {
        if (polls_)
            return static_cast<uint16_t>( (static_cast<float>(works_) / polls_) * g_precision );
        else
            return 0;
    }
    // T2: End

    void clear()
    {
        if (controlFlags_ & ControlFlags::Clear)
        {
            /* Debug information 
            std::cout << "Overhead = " << overhead_ << std::endl;
            std::cout << "Duty     = " << saturation_ << std::endl;

            std::cout << "works_ = " << works_ << std::endl;
            std::cout << "polls_ = " << polls_ << std::endl;

            std::cout << "start_ = " << start_ << std::endl;
            std::cout << "end_   = " << end_ << std::endl;
            std::cout << "end_ - start_ = " << end_ - start_ << std::endl;
            // */

            //std::cout << "Clearing" << std::endl;
            overhead_       = 0;
            saturation_     = 0;
            polls_          = 0;
            works_          = 0;
            controlFlags_   &= ~ControlFlags::Clear;
        }
    }

    void addOverhead (uint64_t o)
    {
        overhead_ += o;
        ++polls_;
    }

    void addDuty(uint64_t d)
    {
        saturation_ += d;
        ++works_;
    }

	bool cleared()
	{
		return controlFlags_ & ControlFlags::Clear;
	}

	void setClear()
	{
        start_ = end_ = getcc_ns();
		controlFlags_ |= ControlFlags::Clear;
	}

    struct CheckPoint
    {
        CheckPoint(CycleTracker& ct, ResultsSync& rs) : ct_(ct), rs_(rs)
        {
        }

        ~CheckPoint()
        {
            ct_.clear();

            if (p2_)
                ct_.addOverhead(p2_ - p1_);
            if (p3_)
                ct_.addDuty(p3_ - p2_);

            ct_.calcResults(rs_);
        }

        void markOne() { p1_ = getcc_ns(); }
        void markTwo() { p2_ = getcc_ns(); }
        void markThree() { p3_ = getcc_ns(); }

        CycleTracker& ct_;
        ResultsSync& rs_;

        uint64_t p1_{0};
        uint64_t p2_{0};
        uint64_t p3_{0};
    };
};
///////////////////////////////////////////////////////////////////////////////

template <typename T, typename Q>
void producer(Q* q, uint32_t iterations, uint64_t workCycles, uint32_t workIterations)
{
	while (Thread::g_pstart.load() == false) {}

	T d;

    d.get().workCycles = workCycles;
    d.get().workIterations = workIterations;

	bool work = false;

	for ( uint32_t i = 0; i < iterations; ++i)
	{
		do 
		{ 
			work = (q->push(d));
			if(!work)
				__builtin_ia32_pause();
		} while (!work); 
		
        //uint64_t start = getcc_ns(); 
        //while (getcc_ns() - start < 119600){} // 200ns on 3GHz CPU.
	}
}

// EX2: Begin
template <typename T, typename Q, typename WD>
void consumer(Q* q, int32_t iterations, ResultsSync& rs, CycleTracker& ct, WD& wd)
{
	while (Thread::g_cstart.load() == false) {}

	T d;
	uint64_t start;
	bool work = false;

    ct.start();
    for (;;)
    {
        CycleTracker::CheckPoint cp(ct, rs);
        cp.markOne(); // roll into CheckPoint constructor?

        start = getcc_ns();
        work = q->pop(d);
        if (!work)
        {
            cp.markTwo();
            __builtin_ia32_pause();
            continue;
        }
        cp.markTwo();

        // simulate work
        for (uint32_t k = 0; k < d.get().workIterations; k++)
        {
			// get a local copy of data
			WD local_wd(wd);
            for (uint32_t it = 0; it < WriteWorkData::Elem; ++it)
            {
                // simulate work on data
                while (getcc_ns() - start < d.get().workCycles){}
                // simulate writing results
                wd.wwd.data[it]++;
            }
        }
        cp.markThree();
    }
}
// EX3: Begin
//
template <typename WD>
void worker(WD& wd)
{
    std::cout << "Launched worker" << std::endl;
    // simulate work
    uint32_t producer_results[WriteWorkData::Elem];
    for (;;)
    {
        for (uint32_t it = 0; it < WriteWorkData::Elem; ++it)
        {
            // simulate consuming producers results
            producer_results[it] = wd.wwd.data[it].load();
        }

        // simulate processing on results from producers
        uint64_t start = getcc_ns();
        while (getcc_ns() - start < 900){} // 300ns on 3GHz CPU
        for (uint32_t it = 0; it < ReadWorkData::Elem; ++it)
        {
            // simulate writing for producers to consume
            wd.rwd.data[it] =  producer_results[it];
        }
    }
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
void run ( const std::string& pc, uint64_t workCycles, uint32_t workIterations )
{
    using WD_t = WorkData<alignof(T)>;
    WD_t wd;

	std::cout	<< "Alignment of T " 
				<< alignof(T) 
				<< std::endl;

	std::vector<std::unique_ptr<std::thread>> 
		threads;
	
	threads.reserve(pc.length());

    auto rs = std::make_unique<Alignment<ResultsSync, alignof(T)>[]>(pc.length());
    auto ct = std::make_unique<Alignment<CycleTracker, alignof(T)>[]>(pc.length());

	Q<T> q(128);

	// need to make this a command line option 
	// and do proper balancing between 
	// consumers and producers
	uint32_t iterations = 1000000000;

    uint32_t core{0};
    uint32_t index{0};
    for (auto i : pc)
    {
        if (i == 'p')
        {
            threads.push_back(
                    std::make_unique<std::thread>
                    (producer<T,Q<T>>
                     , &q 
                     , iterations
                     , workCycles
                     , workIterations));
            setAffinity(*threads.rbegin(), core);
        }
        else if (i == 'c')
        {
            threads.push_back(
                    std::make_unique<std::thread>		  
                    (consumer<T,Q<T>,WD_t>
                     , &q
                     , iterations
                     , std::ref(rs[index].get())
                     , std::ref(ct[index].get())
                     , std::ref(wd)));

            // adjust for physical cpu/core layout
            setAffinity(*threads.rbegin(), core);
            ++index;
        }
        else if (i == 'w')
        {
            threads.push_back(
                    std::make_unique<std::thread>		  
                    (worker<WD_t>, 
                     std::ref(wd)));

            // adjust for physical cpu/core layout
            setAffinity(*threads.rbegin(), core);
        }

        ++core;
    }

	Thread::g_cstart.store(true);
	usleep(500000);
	Thread::g_pstart.store(true);

    auto results = std::make_unique<Results[]>(index);

    for (int t = 0; t < 3600; ++t)
    {
        sleep(1);
        for ( uint32_t i = 0; i < index; ++i)
            results[i] = ct[i].get().getResults(rs[i].get(), true);

        uint64_t totalBandwidth{0};
        std::cout << "----" << std::endl;
        std::cout << "workCycles = " << workCycles << std::endl;
        std::cout << "workIterations = " << workIterations << std::endl;
        for ( uint32_t i = 0; i < index; ++i)
        {
            // need to make fetcher methods to hide the g_precision and ugly static_cast
            // T1 Begin
            std::cout << "saturation [Cycles] = " << results[i].saturationCycles() << std::endl;
            std::cout << "saturation [Ratio] =  " << results[i].saturationRatio() << std::endl;
            std::cout << "Bandwidth [work/ms] = " << results[i].bandwidth() << std::endl;
            totalBandwidth += results[i].bandwidth();
            // T1 End
        }
        std::cout << "Total Bandwidth = " << totalBandwidth << std::endl;
        std::cout << "----" << std::endl << std::endl;

    }

    
	for (auto& i : threads)
	{
		i->join();
	}
}

int main ( int argc, char* argv[] )
{
	if (argc < 3)
	{
		std::cout	<< "Usage: " 
					<< argv[0] 
					<< " <cl|nocl|SimpleCL|SimpleNOCL> "
					"<producer/consumer string (01ppcc67)> " 
                    "[optional] <work cycles> default=6000"
                    "[optional] <work iterations> default=10"
					<< std::endl;
		return 0;
	}

    uint32_t workCycles = 6000; // 2us on 3GHz box

    if (argc >= 4)
        workCycles = atoi(argv[3]);

    uint32_t workIterations = 10; 

    if (argc >= 5)
        workIterations = atoi(argv[4]);


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

    std::cout << "workCycles = " << workCycles << std::endl;


	std::string cl(argv[1]);
		
	if (cl == "cl")
	{
		run<Alignment<
			  Benchmark, 64>
			, boost::lockfree::queue> 
                (pc, workCycles, workIterations);
	}
	else if (cl == "nocl")
	{
		run<Alignment<
			  Benchmark 
			, alignof(Benchmark)>
			, boost::lockfree::bad_queue>
                (pc, workCycles, workIterations);
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

// EX1: Begin
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
        for (i = 0; i < idx; ++i)
        {
            counters[i] = data[i].d.load();
            data[i].d.store(0);
        }


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
// EX1: End

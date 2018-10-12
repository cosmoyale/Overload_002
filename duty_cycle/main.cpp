#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <array>
#include <unistd.h>

#include "getcc.h"
////////////////////////////////////////////////////////////////////////////////
//

constexpr uint32_t g_precision = 10000;

template <typename T, int X>
struct Alignment
{
    alignas(X) T t_;
    T& get() { return t_; }
    operator T () {return t_;}
};

struct Results
{
    Results() {}
    uint32_t bandwidth_{0}; // return raw messages per result?
    uint16_t saturationCycles_{0};
    uint16_t saturationRatio_{0};
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
    // as long as getResults is called infrequently, at most once every 10ms
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


    // one billion is once per second
    // one millino is once per millisecond
    // one thousand is once per microsecond
    uint32_t bandwidth(uint32_t per = 1'000'000'000)
    {
        // 3 is CPU speed in GHz (needs to be set per host)
        return (static_cast<float>((works_)*3.0*per) / (end_ - start_));//*g_precision;
    }

    uint16_t saturationCycles()
    {
        if (saturation_)
            return static_cast<uint16_t>(   (static_cast<float>(saturation_) / (saturation_ + overhead_))* g_precision    );
        else
            return 0;
    }

    uint16_t saturationRatio()
    {
        if (polls_)
            return static_cast<uint16_t>( (static_cast<float>(works_) / polls_) * g_precision );
        else
            return 0;
    }

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

void worker ( CycleTracker& ct, ResultsSync& rs, int work )
{
    ct.start();
    uint32_t i{0};
    for(;;)
    {
        CycleTracker::CheckPoint cp(ct, rs);
        cp.markOne();
        if (!(i%3)) 
        {
            for(int k = 0; k < 5; ++k)
                getcc_ns();
            __builtin_ia32_pause(); // on failed poll
            cp.markTwo();
            ++i;
            continue;
        }
        cp.markTwo();

        // simulate work
        for(int k = 0; k < work; ++k)
            getcc_ns();

        ++i;
        cp.markThree();
    }
    ct.end();
}

template <int Align>
class ThreadManager
{
public:
	using AlignedCycleTracker_t = Alignment<CycleTracker,Align>;
	using AlignedResultsSync_t = Alignment<ResultsSync,Align>;

	ThreadManager(uint32_t nThreads) : nThreads_(nThreads)
	{ 
		// We want to be able to dynamically grow, thus *2?
		threads_.reserve(nThreads*2);
		cts_ = std::make_unique<AlignedCycleTracker_t[]>(nThreads*2);
		rs_ = std::make_unique<AlignedResultsSync_t[]>(nThreads*2);
	}

	CycleTracker& getCT ( uint32_t n )
	{
		if (n >= nThreads_)
		{
			throw (std::runtime_error("n out of bounds"));
		}

		return cts_[n];
	}

	ResultsSync& getRS ( uint32_t n )
	{
		if (n >= nThreads_)
		{
			throw (std::runtime_error("n out of bounds"));
		}

		return rs_[n];
	}

	void launchThread ( uint32_t i, int work )
	{
		// race condition on cpuId_, ignore for now
        threads_[i] = std::make_unique<std::thread>(worker, std::ref(cts_[i].get()), std::ref(rs_[i].get()), work);
		setAffinity(threads_[i], cpuId_);
		cpuId_ += 1;
	}

    Results getResults(uint32_t i)
    {
        ResultsSync r;

        // false sharing other thread
        if (cts_[i].get().cleared())
        {
            return r.results_;
        }

        r = rs_[i].get();
        cts_[i].get().setClear();
        return r.results_;
    }

	ThreadManager() = delete;

private:
	uint32_t nThreads_{0};
	uint32_t cpuId_{0};
    std::vector<std::unique_ptr<std::thread>> threads_;
	std::unique_ptr<AlignedCycleTracker_t[]> cts_;
	std::unique_ptr<AlignedResultsSync_t[]> rs_;
	
};

template <int Align>
void run (int work, int nThreads)
{
	ThreadManager<Align> tm(nThreads);

    // this ends up being the point of contention

    for (int i = 0; i < nThreads; ++i)
    {
		tm.launchThread(i, work);
    }

    for (;;)
    {
        sleep(1);

        // begin observation
        Results r[nThreads];
        for (int i = 0; i < nThreads; ++i)
        {
            r[i] = tm.getResults(i);
        }

		uint64_t totalBandwidth{0};

		std::cout << "Sizeof CycleTracker = " << sizeof(CycleTracker) << std::endl;
		std::cout << "Sizeof ResultSync = " << sizeof(ResultsSync) << std::endl;
		std::cout << "Sizeof AlignedCycleTracker = " << sizeof(typename ThreadManager<Align>::AlignedCycleTracker_t) << std::endl;
		std::cout << "Sizeof AlignedResultSync = " << sizeof(typename ThreadManager<Align>::AlignedCycleTracker_t) << std::endl;
		std::cout << "Sizeof Results = " << sizeof(Results) << std::endl;
		std::cout << "Sizeof align = " << Align << std::endl;
		std::cout << "Work = " << work << std::endl;
           
        for ( int i = 0; i < nThreads; ++i)
        {

            // need to make fetcher methods to hide the g_precision and ugly static_cast
            std::cout << "saturation [Cycles] = " << static_cast<float>(r[i].saturationCycles_)/g_precision << std::endl;
            std::cout << "saturation [Ratio] =  " << static_cast<float>(r[i].saturationRatio_)/g_precision << std::endl;
            std::cout << "Bandwidth [work/ms] = " << static_cast<float>(r[i].bandwidth_)  << std::endl;
			totalBandwidth += r[i].bandwidth_;

        }
		std::cout << "Total Bandwidth = " << totalBandwidth << std::endl;
		std::cout << "----" << std::endl << std::endl;
    }

}

int main ( int argc, char* argv[] )
{
    int work{50};
	int nThreads = 8;

    if (argc > 1)
    {
        work = atoi(argv[1]);
    }

	if (argc > 2)
		nThreads = atoi(argv[2]);


	if (argc > 3)
	{
		int align = atoi(argv[3]);

		std::cout << "--- align = " << align << std::endl;

		if (align == 4)
			run<4>(work, nThreads);
		if (align == 8)
			run<8>(work, nThreads);
		if (align == 64)
			run<64>(work, nThreads);
	}
	else
	{
		run<64>(work, nThreads);
	}


  	return 0;
}

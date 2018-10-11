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
    uint16_t saturationCycles_{0};
    uint16_t saturationRatio_{0};
    uint32_t bandwidth_{0}; // return raw messages per result?
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


    ResultsSync rs_;
    Results& results_{rs_.results_};

    uint64_t start_{0};
    uint64_t end_{0};
    uint64_t overhead_{0};
    uint64_t saturation_{0};
    uint32_t polls_{0};
    uint32_t works_{0};

    // There is intentional false sharing on this, however the impact is unmasurable
    // as long as getResults is called infrequently, at most once every 10ms
    std::atomic<uint32_t> controlFlags_{0};

    CycleTracker() : results_(rs_.results_) {}


    void start()
    {
        start_ = getcc_ns();
    }

    void end()
    {
        end_ = getcc_ns();
    }

    void calcResults()
    {
        end();
        results_.saturationCycles_ = saturationCycles();
        results_.saturationRatio_ = saturationRatio();
        results_.bandwidth_ = bandwidth(1'000'000);//(end_ - start_) * works_;
    }

    Results getResults()
    {
        ResultsSync r;

        // false sharing other thread
        if (controlFlags_ & ControlFlags::Clear)
        {
            return r.results_;
        }

        start_ = end_ = getcc_ns();
        r = rs_;
        controlFlags_ |= ControlFlags::Clear;
        return r.results_;
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

    struct CheckPoint
    {
        CheckPoint(CycleTracker& ct) : ct_(ct)
        {
        }

        ~CheckPoint()
        {
            ct_.clear();

            if (p2_)
                ct_.addOverhead(p2_ - p1_);
            if (p3_)
                ct_.addDuty(p3_ - p2_);

            ct_.calcResults();
        }

        void markOne() { p1_ = getcc_ns(); }
        void markTwo() { p2_ = getcc_ns(); }
        void markThree() { p3_ = getcc_ns(); }

        CycleTracker& ct_;

        uint64_t p1_{0};
        uint64_t p2_{0};
        uint64_t p3_{0};
    };
};

void worker ( CycleTracker& ct, int work )
{
    ct.start();
    uint32_t i{0};
    for(;;)
    {
        CycleTracker::CheckPoint cp(ct);
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

int main ( int argc, char* argv[] )
{
    int work{50};

    if (argc > 1)
    {
        work = atoi(argv[1]);
    }

    constexpr int nThreads = 10;
    std::array<std::unique_ptr<std::thread>, nThreads> threads;

    // this ends up being the point of contention
    constexpr int align = 64;
    std::array<Alignment<CycleTracker, align>, nThreads> cts;

    for (int i = 0; i < nThreads; ++i)
    {
        threads[i] = std::make_unique<std::thread>(worker, std::ref(cts[i].get()), work);
    }


    for (;;)
    {
        sleep(1);

        // begin observation
        Results r[nThreads];
        for (int i = 0; i < nThreads; ++i)
        {
            r[i] = cts[i].get().getResults();
        }

        for ( int i = 0; i < 10; ++i)
        {

            std::cout << "Sizeof CycleTracker = " << sizeof(CycleTracker) << std::endl;
            std::cout << "Sizeof Results = " << sizeof(Results) << std::endl;
           
            // need to make fetcher methods to hide the g_precision and ugly static_cast
            std::cout << "saturation [Cycles] = " << static_cast<float>(r[i].saturationCycles_)/g_precision << std::endl;
            std::cout << "saturation [Ratio] =  " << static_cast<float>(r[i].saturationRatio_)/g_precision << std::endl;
            std::cout << "Bandwidth [work/ms] = " << static_cast<float>(r[i].bandwidth_)  << std::endl;

            std::cout << "----" << std::endl << std::endl;
        }
    }

  	return 0;
}

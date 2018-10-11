#include <iostream>
#include <atomic>
#include <thread>
#include <unistd.h>

#include "getcc.h"
////////////////////////////////////////////////////////////////////////////////
//

constexpr uint32_t g_precision = 10000;
struct Results
{

    Results() {}
    uint16_t saturationCycles_{0};
    uint16_t saturationRatio_{0};
    uint32_t bandwidth_{0}; // return raw messages per time quantum or break down in to Kila,Mega,Giga?
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
        results_.bandwidth_ = bandwidth();//(end_ - start_) * works_;
    }

    Results getResults()
    {
        start_ = end_ = getcc_ns();
        ResultsSync r;
        r = rs_;
        controlFlags_ |= ControlFlags::Clear;
        return r.results_;
    }

    uint64_t bandwidth()
    {
        // busy wait on clear flag
        // 3 is CPU speed in GHz
        // 1'000'000 is 1 ms
        return (static_cast<float>((works_)*3.0*1'000'000) / (end_ - start_));//*g_precision;
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
            return static_cast<uint16_t>((works_*g_precision) / polls_);
        else
            return 0;
    }

    void clear()
    {
        if (controlFlags_ & ControlFlags::Clear)
        {
             
            std::cout << "Overhead = " << overhead_ << std::endl;
            std::cout << "Duty     = " << saturation_ << std::endl;

            std::cout << "works_ = " << works_ << std::endl;
            std::cout << "polls_ = " << polls_ << std::endl;

            std::cout << "start_ = " << start_ << std::endl;
            std::cout << "end_   = " << end_ << std::endl;
            std::cout << "end_ - start_ = " << end_ - start_ << std::endl;

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

        //overhead_.fetch_add(o, std::memory_order_acquire);
        //polls_.fetch_add(1, std::memory_order_release);
    }

    void addDuty(uint64_t d)
    {
        saturation_ += d;
        ++works_;

        //saturation_.fetch_add(d, std::memory_order_acquire);
        //works_.fetch_add(1, std::memory_order_release);
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

void observer ( CycleTracker& ct )
{
    for (int i = 0; i < 10; ++i)
    {
        Results r;
        r = ct.getResults();

        sleep(1);

        std::cout << "Sizeof CycleTracker = " << sizeof(CycleTracker) << std::endl;
        std::cout << "Sizeof Results = " << sizeof(Results) << std::endl;
       
        // need to make fetcher methods to hide the g_precision and ugly static_cast
        std::cout << "saturation [Cycles] = " << static_cast<float>(r.saturationCycles_)/g_precision << std::endl;
        std::cout << "saturation [Ratio] =  " << static_cast<float>(r.saturationRatio_)/g_precision << std::endl;
        std::cout << "Bandwidth [work/ms] = " << static_cast<float>(r.bandwidth_)  << std::endl;

        std::cout << "----" << std::endl << std::endl;

        // after observation 
        ct.clear();
    }
}

int main ( int argc, char* argv[] )
{
    CycleTracker ct;
    std::thread th(observer, std::ref(ct));

    ct.start();
    uint32_t i{0};
    for(;;)
    {
        CycleTracker::CheckPoint cp(ct);
        cp.markOne();
        if (!(i%3)) 
        {
            __builtin_ia32_pause(); // on failed poll
            cp.markTwo();
            ++i;
            continue;
        }
        cp.markTwo();

        for(int k = 0; k < 5000; ++k)
            getcc_ns();

        ++i;
        cp.markThree();
    }
    ct.end();

  	return 0;
}

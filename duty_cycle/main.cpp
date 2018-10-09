#include <iostream>
#include <atomic>
#include "getcc.h"
////////////////////////////////////////////////////////////////////////////////
//

constexpr uint32_t g_precision = 10000;

struct Results
{
    Results() {}
    uint16_t saturationCycles_{0};
    uint16_t saturationRatio_{0};
    uint16_t bandwidth_{0};
    uint8_t controlBits_{0};
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

        //std::atomic<uint64_t>& ac = *reinterpret_cast<std::atomic<uint64_t>*>(&atomicCopy_);
        //std::atomic<uint64_t>& rs_ac = *reinterpret_cast<std::atomic<uint64_t>*>(&rs.atomicCopy_);
        
        std::atomic<uint64_t>& ac = atomicCopy_;
        std::atomic<uint64_t>& rs_ac = rs.atomicCopy_;

        ac.store(rs_ac.load(std::memory_order_acquire), std::memory_order_release);

        return *this;
    }
};

struct CycleTracker
{
    ResultsSync rs_;
    Results& results_{rs_.results_};

    uint64_t start_{0};
    uint64_t end_{0};
    uint64_t overhead_{0};
    uint64_t saturation_{0};
    uint32_t polls_{0};
    uint32_t works_{0};
    uint32_t controlFlags_{0};

    CycleTracker() : results_(rs_.results_) {}

    enum ControlFlags : uint32_t
    {
          Clear     = 1
        , Readable  = 2
    };

    void start()
    {
        start_ = getcc_b();
    }

    void end()
    {
        end_ = getcc_e();
    }

    void calcResults()
    {
        results_.saturationCycles_ = saturationCycles();
        results_.saturationRatio_ = saturationRatio();
        results_.bandwidth_ = (end_ - start_) * works_;
    }

    Results getResults()
    {
        ResultsSync r;
        r = rs_;
        return r.results_;
    }

    uint64_t bandwidth()
    {
        // busy wait on clear flag
        return (end_ - start_) * works_;
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
            std::cout << "Clearing" << std::endl;
            overhead_       = 0;//(0, std::memory_order_relaxed);
            saturation_     = 0;//(0, std::memory_order_relaxed);
            polls_          = 0;//(0, std::memory_order_relaxed);
            works_          = 0;//(0, std::memory_order_relaxed);
            controlFlags_   &= ~ControlFlags::Clear;
            //controlFlags_.fetch_and(~ControlFlags::Clear, std::memory_order_release);
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

int main ( int argc, char* argv[] )
{
    CycleTracker ct;

    ct.start();
    for (int i = 0; i < 63; ++i)
    {
        CycleTracker::CheckPoint cp(ct);
        cp.markOne();
        if (!(i%3)) 
        {
            __builtin_ia32_pause(); // on failed poll
            cp.markTwo();
            continue;
        }
        cp.markTwo();

        for(int k = 0; k < 5; ++k)
            getcc_ns();
        cp.markThree();
    }
    ct.end();
    ct.calcResults();

    Results r;
    r = ct.getResults();

    std::cout << "Sizeof CycleTracker = " << sizeof(CycleTracker) << std::endl;
    
    std::cout << "Overhead = " << ct.overhead_ << std::endl;
    std::cout << "Duty = " << ct.saturation_ << std::endl;

    std::cout << "works_ = " << ct.works_ << std::endl;
    std::cout << "polls_ = " << ct.polls_ << std::endl;

    std::cout << "start_ = " << ct.start_ << std::endl;
    std::cout << "end_ = " << ct.end_ << std::endl;
    std::cout << "end_ - start_ = " << ct.end_ - ct.start_ << std::endl;

    // need to make fetcher methods to hide the g_precision and ugly static_cast
    std::cout << "saturation [Cycles] = " << static_cast<float>(r.saturationCycles_)/g_precision << std::endl;
    std::cout << "saturation [Ratio] = " << static_cast<float>(r.saturationRatio_)/g_precision << std::endl;
    std::cout << "Bandwidth [work/ms] = " << static_cast<float>(r.bandwidth_) / (3.0 * 1'000'000) << std::endl;

    // after observation 
    ct.clear();
	return 0;
}

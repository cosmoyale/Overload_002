#include <iostream>
#include <atomic>
#include "getcc.h"
////////////////////////////////////////////////////////////////////////////////
//

struct CycleTracker
{
    std::atomic<uint64_t> start_{0};
    std::atomic<uint64_t> end_{0};
    std::atomic<uint64_t> overhead_{0};
    std::atomic<uint64_t> saturation_{0};
    std::atomic<uint32_t> polls_{0};
    std::atomic<uint32_t> works_{0};
    std::atomic<uint32_t> controlFlags_{0};

    enum ControlFlags : uint32_t
    {
          Clear     = 1
        , Readable  = 2
    };

    void start()
    {
        start_.store(getcc_b());
    }

    void end()
    {
        end_.store(getcc_e());
    }

    uint64_t bandwidth()
    {
        return (end_ - start_) * works_;
    }

    float saturationCycles()
    {
        if (saturation_)
            return static_cast<float>(saturation_) / (saturation_ + overhead_);
        else
            return 0;
    }

    float saturationRatio()
    {
        if (polls_)
            return static_cast<float>(works_) / polls_;
        else
            return 0;
    }

    void clear()
    {
        if (controlFlags_.load(std::memory_order_acquire) & ControlFlags::Clear)
        {
            std::cout << "Clearing" << std::endl;
            overhead_.store     (0, std::memory_order_relaxed);
            saturation_.store   (0, std::memory_order_relaxed);
            polls_.store        (0, std::memory_order_relaxed);
            works_.store        (0, std::memory_order_relaxed);
            controlFlags_.fetch_and(~ControlFlags::Clear, std::memory_order_release);
        }
    }

    void addOverhead (uint64_t o)
    {
        overhead_.fetch_add(o, std::memory_order_acquire);
        polls_.fetch_add(1, std::memory_order_release);
    }

    void addDuty(uint64_t d)
    {
        saturation_.fetch_add(d, std::memory_order_acquire);
        works_.fetch_add(1, std::memory_order_release);
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
            {
                ct_.addOverhead(p2_ - p1_);
            }
            if (p3_)
            {
                ct_.addDuty(p3_ - p2_);
            }
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

        for(int k = 0; k < 200; ++k)
            getcc_ns();
        cp.markThree();
    }
    ct.end();

    std::cout << "Sizeof CycleTracker = " << sizeof(CycleTracker) << std::endl;
    std::cout << "Overhead = " << ct.overhead_ << std::endl;
    std::cout << "Duty = " << ct.saturation_ << std::endl;

    std::cout << "work saturation [Cycles] = " << ct.saturationCycles() << std::endl;
    std::cout << "ratio saturation [Ratio] = " << ct.saturationRatio() << std::endl;
    std::cout << "Bandwidth [work/ms] = " << ct.bandwidth() / (3.0 * 1'000'000) << std::endl;
	return 0;
}

#ifndef OWD_CALC_H
#define OWD_CALC_H

#include <algorithm>
#include <vector>

namespace dombft::OWDCalc {

class BaseCalcCtx {
public:
    BaseCalcCtx(uint32_t numReceivers, uint32_t cap, uint32_t windowSize, uint32_t initialMeasure = 0)
        : numReceivers_(numReceivers)
        , cap_(cap)
        , windowSize_(windowSize)
        , windowIndex_(numReceivers, 0)
    {
        recvrMeasures_.resize(numReceivers_);
        for (auto &measures : recvrMeasures_) {
            measures.resize(windowSize, initialMeasure);
        }
    };

    virtual inline void addMeasure(uint32_t rcvrIndex, uint32_t measure) = 0;

    uint32_t getCappedMaxOWD() const
    {
        uint32_t maxOWD = 0;
        for (auto &measures : recvrMeasures_) {
            maxOWD = std::min(cap_, std::max(maxOWD, getRcvrOWD(measures)));
        }

        if (VLOG_IS_ON(6)) {
            std::ostringstream out;
            for (size_t i = 0; i < recvrMeasures_.size(); i++) {
                out << i << " | ";
                for (uint32_t measure : recvrMeasures_[i]) {
                    out << measure << " ";
                }
                out << "\n";
            }
            VLOG(6) << out.str();
        }

        return maxOWD;
    }
    uint32_t getPercentileOWD(uint32_t percentile) const
    {
        uint32_t percentileOWD = 0;
        std::vector<uint32_t> sortedMeasure;
        for (auto &measures : recvrMeasures_) {
            sortedMeasure.push_back(getRcvrOWD(measures));
        }
        std::sort(sortedMeasure.begin(), sortedMeasure.end());
        percentileOWD = sortedMeasure[(sortedMeasure.size() - 1) * percentile / 100];
        return percentileOWD;
    }

protected:
    virtual uint32_t getRcvrOWD(const std::vector<uint32_t> &rcvrMeasures) const = 0;

protected:
    uint32_t numReceivers_;
    uint32_t cap_;
    uint32_t windowSize_;

    std::vector<uint32_t> windowIndex_;
    std::vector<std::vector<uint32_t>> recvrMeasures_;
};

class MeanCtx : public BaseCalcCtx {
public:
    explicit MeanCtx(uint32_t numReceivers, uint32_t cap, uint32_t windowSize, uint32_t initialMeasure = 0)
        : BaseCalcCtx(numReceivers, cap, windowSize, initialMeasure)
    {
    }

    inline void addMeasure(uint32_t rcvrIndex, uint32_t measure) override
    {
        recvrMeasures_[rcvrIndex][windowIndex_[rcvrIndex]] = measure;
        windowIndex_[rcvrIndex] = windowIndex_[rcvrIndex] + 1;
    }

private:
    inline uint32_t getRcvrOWD(const std::vector<uint32_t> &measures) const
    {
        uint32_t sum = 0;
        for (auto &measure : measures) {
            sum += measure;
        }
        return sum / measures.size();
    }
};

class MaxCtx : public BaseCalcCtx {
public:
    explicit MaxCtx(uint32_t numReceivers, uint32_t cap, uint32_t initialMeasure = 0)
        : BaseCalcCtx(numReceivers, cap, 1, initialMeasure)
    {
    }

    inline void addMeasure(uint32_t rcvrIndex, uint32_t measure) override
    {
        recvrMeasures_[rcvrIndex][0] = std::max(recvrMeasures_[rcvrIndex][0], measure);
    }

private:
    inline uint32_t getRcvrOWD(const std::vector<uint32_t> &measures) const override { return measures[0]; }
};

class PercentileCtx : public BaseCalcCtx {
public:
    explicit PercentileCtx(
        uint32_t numReceivers, uint32_t cap, uint32_t windowSize, uint32_t percentile, uint32_t initialMeasure = 0
    )
        : BaseCalcCtx(numReceivers, cap, windowSize, initialMeasure)
        , percentile_(percentile)
    {
    }

    inline void addMeasure(uint32_t rcvrIndex, uint32_t measure) override
    {
        recvrMeasures_[rcvrIndex][windowIndex_[rcvrIndex]] = measure;
        windowIndex_[rcvrIndex] = windowIndex_[rcvrIndex] + 1;
    }

private:
    uint32_t percentile_;
    uint32_t getRcvrOWD(const std::vector<uint32_t> &measures) const override
    {
        std::vector<uint32_t> sortedMeasure = measures;
        std::sort(sortedMeasure.begin(), sortedMeasure.end());
        return sortedMeasure[(sortedMeasure.size() - 1) * percentile_ / 100];
    }
};
}   // namespace dombft::OWDCalc
#endif
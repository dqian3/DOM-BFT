#ifndef OWD_CALC_H
#define OWD_CALC_H

// TODO get rid of templates here..
// The main issue with this design is that the Strategy class acts as both 
// a template for constructing per-receiver strategies and a calculator for
// those strategies. It works for now though.

#include <vector>
#include <algorithm>
namespace dombft {
    namespace OWDCalc {
        // A Strategy pattern
        class BaseCalcStrategy {
        public:
            virtual ~BaseCalcStrategy() = default;

            virtual void addMeasure(uint32_t measure) = 0;

            virtual uint32_t getOWD() const = 0;
        };

        class MeanStrategy : public BaseCalcStrategy {
        public:
            explicit MeanStrategy(uint32_t windowSize = 0) :
                    windowSize_(windowSize), windowIndex_(0) { storedMeasure_.resize(windowSize); }

            inline void addMeasure(uint32_t measure) override {
                storedMeasure_[windowIndex_] = measure;
                windowIndex_ = windowSize_ ? (windowIndex_ + 1) % windowSize_ : windowIndex_ + 1;
            }

            inline uint32_t getOWD() const override {
                uint32_t sum = 0;
                for (auto &measure: storedMeasure_) {
                    sum += measure;
                }
                return sum / storedMeasure_.size();
            }

        private:
            uint32_t windowSize_;
            uint32_t windowIndex_;
            std::vector<uint32_t> storedMeasure_;
        };

        class MaxStrategy : public BaseCalcStrategy {
        public:
            inline void addMeasure(uint32_t measure) override {
                storedMeasure_ = std::max(storedMeasure_, measure);
            }

            inline uint32_t getOWD() const override {
                return storedMeasure_;
            }

        private:
            uint32_t storedMeasure_;
        };

        class PercentileStrategy : public BaseCalcStrategy {
        public:
            explicit PercentileStrategy(uint32_t percentile, uint32_t windowSize = 0) :
                    windowSize_(windowSize), percentile_(percentile), windowIndex_(0) {
                storedMeasure_.resize(windowSize);
            }

            inline void addMeasure(uint32_t measure) override {
                storedMeasure_[windowIndex_] = measure;
                windowIndex_ = windowSize_ ? (windowIndex_ + 1) % windowSize_ : windowIndex_ + 1;
            }

            inline uint32_t getOWD() const override {
                std::vector<uint32_t> sortedMeasure = storedMeasure_;
                std::sort(sortedMeasure.begin(), sortedMeasure.end());
                return sortedMeasure[(sortedMeasure.size() - 1) * percentile_ / 100];
            }

        private:
            uint32_t windowSize_;
            uint32_t percentile_;
            uint32_t windowIndex_;
            std::vector<uint32_t> storedMeasure_;
        };


        template<typename T>    
        class MeasureContext {
        public:
            MeasureContext(uint32_t numReceivers, T strategy, uint32_t cap, uint32_t windowSize = 0) :
                    numReceivers_(numReceivers), cap_(cap), windowSize_(windowSize) {
                for (uint32_t i = 0; i < numReceivers_; i++) {
                    receiverOWDs_.push_back(std::make_unique<T>(strategy));
                }

            }

            inline void addMeasure(uint32_t rcvrIndex, uint32_t measure) {
                receiverOWDs_[rcvrIndex]->addMeasure(measure);
            }

            inline uint32_t getOWD() const {
                // find the largest OWD among all receivers that is smaller than cap
                uint32_t maxOWD = 0;
                for (uint32_t i = 0; i < numReceivers_; i++) {
                    uint32_t curOWD = receiverOWDs_[i]->getOWD();
                    maxOWD = curOWD >= cap_ ? maxOWD : std::max(maxOWD, curOWD);
                }

                return maxOWD;
            }

        private:
            uint32_t numReceivers_;
            uint32_t cap_;
            uint32_t windowSize_;
            std::vector<std::unique_ptr<BaseCalcStrategy>> receiverOWDs_;
        };

    }
}

#endif
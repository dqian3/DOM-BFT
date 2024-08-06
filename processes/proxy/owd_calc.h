#ifndef OWD_CALC_H
#define OWD_CALC_H

#include <vector>
#include <algorithm>

namespace dombft::OWDCalc {

        class BaseCalcCtx {
        public:
            BaseCalcCtx(uint32_t numReceivers, uint32_t cap, uint32_t windowSize, uint32_t initialMeasure = 0) :
                    numReceivers_(numReceivers), cap_(cap),windowSize_(windowSize), windowIndex_(0)
                    {
                        recvrMeasures_.resize(numReceivers_);
                        for(auto &measures: recvrMeasures_) {
                            measures.resize(windowSize, initialMeasure);
                        }
                    };

            virtual inline void addMeasure(uint32_t rcvrIndex, uint32_t measure) = 0;

            // TODO(Hao): add a function to get the OWD with heuristics
            virtual inline uint32_t getOWD() const = 0;

        protected:
            uint32_t numReceivers_;
            uint32_t cap_;
            uint32_t windowSize_;
            uint32_t windowIndex_;
            std::vector<std::vector<uint32_t>> recvrMeasures_;
        };


        class MeanCtx : public BaseCalcCtx {
        public:
            explicit MeanCtx(uint32_t numReceivers, uint32_t cap, uint32_t windowSize, uint32_t initialMeasure = 0)
                : BaseCalcCtx(numReceivers, cap,windowSize,initialMeasure){}

            inline void addMeasure(uint32_t rcvrIndex, uint32_t measure) override {
                recvrMeasures_[rcvrIndex][windowIndex_] = measure;
                windowIndex_ = windowSize_ ? (windowIndex_ + 1) % windowSize_ : windowIndex_ + 1;
            }

            inline uint32_t getOWD() const override {
                uint32_t  maxOWD = 0;
                for(auto &measures: recvrMeasures_) {
                    maxOWD = std::min(cap_,std::max(maxOWD, getMean(measures)));
                }
                return maxOWD;
            }

        private:
            inline uint32_t getMean(const std::vector<uint32_t> &measures) const {
                uint32_t sum = 0;
                for (auto &measure: measures) {
                    sum += measure;
                }
                return sum / measures.size();
            }
        };

        class MaxCtx : public BaseCalcCtx {
        public:
            explicit MaxCtx(uint32_t numReceivers, uint32_t cap, uint32_t initialMeasure = 0)
            : BaseCalcCtx(numReceivers, cap,1,initialMeasure){}

            inline void addMeasure(uint32_t rcvrIndex, uint32_t measure) override {
                recvrMeasures_[rcvrIndex][0] = std::max(recvrMeasures_[rcvrIndex][0], measure);
            }

            inline uint32_t getOWD() const override {
                uint32_t  maxOWD = 0;
                for(auto &measures: recvrMeasures_) {
                    maxOWD = std::min(cap_,std::max(maxOWD, measures[0]));
                }
                return maxOWD;
            }
        };

        class PercentileCtx : public BaseCalcCtx {
        public:
            explicit PercentileCtx(uint32_t numReceivers, uint32_t cap, uint32_t windowSize,uint32_t percentile, uint32_t initialMeasure = 0)
                    : BaseCalcCtx(numReceivers, cap,windowSize,initialMeasure), percentile_(percentile){}

            inline void addMeasure(uint32_t rcvrIndex, uint32_t measure) override {
                recvrMeasures_[rcvrIndex][windowIndex_] = measure;
                windowIndex_ = windowSize_ ? (windowIndex_ + 1) % windowSize_ : windowIndex_ + 1;
            }

            inline uint32_t getOWD() const override {
                uint32_t  maxOWD = 0;
                for(auto &measures: recvrMeasures_) {
                    maxOWD = std::min(cap_,std::max(maxOWD, getPercentile(measures)));
                }
                return maxOWD;
            }

        private:
            uint32_t percentile_;
            uint32_t getPercentile(const std::vector<uint32_t> &measures) const {
                std::vector<uint32_t> sortedMeasure = measures;
                std::sort(sortedMeasure.begin(), sortedMeasure.end());
                return sortedMeasure[(sortedMeasure.size() - 1) * percentile_ / 100];
            }
        };
    }


#endif
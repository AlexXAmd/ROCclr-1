//
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
//
#include "device/pal/palkernel.hpp"
#include "device/pal/palwavelimiter.hpp"
#include "os/os.hpp"
#include "utils/flags.hpp"

#include <cstdlib>
using namespace std;

namespace pal {

uint WaveLimiter::MaxWave;
uint WaveLimiter::WarmUpCount;
uint WaveLimiter::RunCount;
uint WLAlgorithmSmooth::AdaptCount;
uint WLAlgorithmSmooth::AbandonThresh;
uint WLAlgorithmSmooth::DscThresh;

WaveLimiter::WaveLimiter(
        HSAILKernel* owner,
        uint    seqNum,
        bool    enable,
        bool    enableDump):
        owner_(owner),
        dumper_(owner_->name() + "_" + std::to_string(seqNum), enableDump) {
    auto gpuDev = static_cast<const Device*>(&owner_->dev());
Unimplemented();
    //auto attrib = gpuDev->getAttribs();
    auto hwInfo = gpuDev->hwInfo();
    setIfNotDefault(SIMDPerSH_, GPU_WAVE_LIMIT_CU_PER_SH,
            /*attrib.numberOfCUsperShaderArray*/ 8 * hwInfo->simdPerCU_);
    MaxWave = GPU_WAVE_LIMIT_MAX_WAVE;
    WarmUpCount = GPU_WAVE_LIMIT_WARMUP;
    RunCount = GPU_WAVE_LIMIT_RUN * MaxWave;

    state_ = WARMUP;
    if (!flagIsDefault(GPU_WAVE_LIMIT_TRACE)) {
        traceStream_.open(std::string(GPU_WAVE_LIMIT_TRACE) + owner_->name() +
            ".txt");
    }

    waves_ = MaxWave;
    currWaves_ = MaxWave;
    bestWave_ = MaxWave;
    enable_ = enable;
}

WaveLimiter::~WaveLimiter() {
    if (traceStream_.is_open()) {
        traceStream_.close();
    }
}

uint WaveLimiter::getWavesPerSH(){
    currWaves_ = waves_;
    return waves_ * SIMDPerSH_;
}

WLAlgorithmSmooth::WLAlgorithmSmooth(HSAILKernel* owner, uint seqNum, bool enable, bool enableDump):
    WaveLimiter(owner, seqNum, enable, enableDump) {
    AdaptCount = 2 * MaxWave + 1;
    AbandonThresh = GPU_WAVE_LIMIT_ABANDON;
    DscThresh = GPU_WAVE_LIMIT_DSC_THRESH;

    dynRunCount_ = RunCount;
    measure_.resize(MaxWave + 1);
    reference_.resize(MaxWave + 1);
    trial_.resize(MaxWave + 1);
    ratio_.resize(MaxWave + 1);

    clearData();
}

WLAlgorithmSmooth::~WLAlgorithmSmooth() {

}

void WLAlgorithmSmooth::clearData() {
    waves_ = MaxWave;
    countAll_ = 0;
    clear(measure_);
    clear(reference_);
    clear(trial_);
    clear(ratio_);
    discontinuous_ = false;
    dataCount_ = 0;
}

void WLAlgorithmSmooth::updateData(ulong time) {
    auto count = dataCount_ - 1;
    assert(count < 2 * MaxWave + 1);
    assert(time > 0);
    assert(currWaves_ == waves_);
    if (count % 2 == 0) {
        assert(waves_ == MaxWave);
        auto pos = count / 2;
        measure_[pos] = time;
        if (pos > 0) {
            auto wave = MaxWave + 1 - pos;
            if (abs(static_cast<long>(measure_[pos - 1]) -
                    static_cast<long>(measure_[pos])) * 100 / measure_[pos] >
                    DscThresh) {
                discontinuous_ = true;
            }
            reference_[wave] = (time + measure_[pos - 1]) / 2;
            ratio_[wave] = trial_[wave] * 100 / reference_[wave];
            if (ratio_[bestWave_] > ratio_[wave] && !discontinuous_) {
                bestWave_ = wave;
            }
        }
    } else {
        assert(waves_ == MaxWave - count / 2);
        trial_[waves_] = time;
    }
    outputTrace();
}

void WLAlgorithmSmooth::outputTrace() {
    if (!traceStream_.is_open()) {
        return;
    }

    traceStream_ << "[WaveLimiter] " << owner_->name() << " state=" << state_
            << " currWaves=" << currWaves_ << " waves=" << waves_
            << " bestWave=" << bestWave_ << '\n';
    output(traceStream_, "\n measure = ", measure_);
    output(traceStream_, "\n reference = ", reference_);
    output(traceStream_, "\n ratio = ", ratio_);
    traceStream_ << "\n\n";
}


void WLAlgorithmSmooth::callback(ulong duration) {
    dumper_.addData(duration, currWaves_, static_cast<char>(state_));

    if (!enable_) {
        return;
    }

    countAll_++;

    switch (state_) {
    case WARMUP:
        if (countAll_ < WarmUpCount) {
            return;
        }
        state_ = ADAPT;
        bestWave_ = MaxWave;
        clearData();
        return;
    case ADAPT:
        assert(duration > 0);
        if (waves_ == currWaves_) {
            dataCount_++;
            updateData(duration);
            waves_ = MaxWave + 1 - dataCount_ / 2;
            if (dataCount_ == 1 || (dataCount_ < AdaptCount &&
                !discontinuous_ && (dataCount_ % 2 == 0 ||
                ratio_[waves_] < AbandonThresh))) {
                if (dataCount_ % 2 == 1) {
                    --waves_;
                } else {
                    waves_ = MaxWave;
                }
                return;
            }
            waves_ = bestWave_;
            if (dataCount_ >= AdaptCount) {
                dynRunCount_ = RunCount;
            } else {
                dynRunCount_ = AdaptCount;
            }
            countAll_ = rand() % MaxWave;
            state_ = RUN;
        }
        return;
    case RUN:
        if (countAll_ < dynRunCount_) {
            return;
        }
        state_ = ADAPT;
        bestWave_ = MaxWave;
        clearData();
        return;
    }
}

WaveLimiter::DataDumper::DataDumper(const std::string &kernelName, bool enable) {
    enable_ = enable;
    if (enable_) {
        fileName_ = std::string(GPU_WAVE_LIMIT_DUMP) + kernelName + ".csv";
    }
}

WaveLimiter::DataDumper::~DataDumper() {
    if (!enable_) {
        return;
    }

    std::ofstream OFS(fileName_);
    for (size_t i = 0, e = time_.size(); i != e; ++i) {
        OFS << i << ',' << time_[i] << ',' << wavePerSIMD_[i] << ','
            << static_cast<uint>(state_[i]) << '\n';
    }
    OFS.close();
}

void WaveLimiter::DataDumper::addData(ulong time, uint wave, char state) {
    if (!enable_) {
        return;
    }

    time_.push_back(time);
    wavePerSIMD_.push_back(wave);
    state_.push_back(state);
}

WLAlgorithmAvrg::WLAlgorithmAvrg(HSAILKernel* owner, uint seqNum, bool enable, bool enableDump):
    WaveLimiter(owner, seqNum, enable, enableDump) {

    measure_.resize(MaxWave + 1);
    clear(measure_);
    countAll_ = 0;
}

WLAlgorithmAvrg::~WLAlgorithmAvrg() {

}

void WLAlgorithmAvrg::outputTrace() {
    if (!traceStream_.is_open()) {
        return;
    }

    traceStream_ << "[WaveLimiter] " << owner_->name() << " state=" << state_
            << " currWaves=" << currWaves_ << " waves=" << waves_
            << " bestWave=" << bestWave_ << '\n';
    output(traceStream_, "\n measure = ", measure_);
    traceStream_ << "\n\n";
}


void WLAlgorithmAvrg::callback(ulong duration) {
    dumper_.addData(duration, currWaves_, static_cast<char>(state_));

    if (!enable_) {
        return;
    }

    countAll_++;

    switch (state_) {
    case WARMUP:
        state_ = ADAPT;
    case ADAPT:
        measure_[waves_] += duration;
        if (countAll_ <= MaxWave * 5) {
            waves_--;
            if (waves_ == 0) {
                waves_ = MaxWave;
            }
        }
        else {
            bestWave_ = MaxWave;
            for (uint i=1; i<MaxWave; i++ ) {
                if (measure_[i] < measure_[bestWave_]) {
                    bestWave_ = i;
                }
            }
            waves_ = bestWave_;
            state_ = RUN;
        }
        break;
    case RUN:
    default:
        break;
    }
}

WaveLimiterManager::WaveLimiterManager(HSAILKernel* kernel):
        owner_(kernel),
        enable_(false),
        enableDump_(!flagIsDefault(GPU_WAVE_LIMIT_DUMP)) {
    auto gpuDev = static_cast<const Device*>(&owner_->dev());
    Unimplemented();
    //auto attrib = gpuDev->getAttribs();
    auto hwInfo = gpuDev->hwInfo();
    unsigned simdPerSH = 0;
    setIfNotDefault(simdPerSH, GPU_WAVE_LIMIT_CU_PER_SH,
            /*attrib.numberOfCUsperShaderArray*/ 8 * hwInfo->simdPerCU_);
    fixed_ = GPU_WAVES_PER_SIMD * simdPerSH;
}

WaveLimiterManager::~WaveLimiterManager() {
    for (auto &I: limiters_) {
        delete I.second;
    }
}

uint WaveLimiterManager::getWavesPerSH(const device::VirtualDevice *vdev) const {
    if (fixed_ > 0) {
        return fixed_;
    }
    if (!enable_) {
        return 0;
    }
    auto loc = limiters_.find(vdev);
    if (loc == limiters_.end()) {
        return 0;
    }
    assert(loc->second != nullptr);
    return loc->second->getWavesPerSH();
}

amd::ProfilingCallback* WaveLimiterManager::getProfilingCallback(
        const device::VirtualDevice *vdev) {
    assert(vdev != nullptr);
    if (!enable_ && !enableDump_) {
        return nullptr;
    }

    amd::ScopedLock SL(monitor_);
    auto loc = limiters_.find(vdev);
    if (loc != limiters_.end()) {
        return loc->second;
    }

    auto limiter = new WLAlgorithmSmooth(owner_, limiters_.size(), enable_,
            enableDump_);
    if (limiter == nullptr) {
        enable_ = false;
        return nullptr;
    }
    limiters_[vdev] = limiter;
    return limiter;
}

void WaveLimiterManager::enable() {
    if (fixed_ > 0) {
        return;
    }
    auto gpuDev = static_cast<const Device*>(&owner_->dev());
    auto hwInfo = gpuDev->hwInfo();
    Unimplemented();
    // Enable it only for CI+, unless GPU_WAVE_LIMIT_ENABLE is set to 1
    // Disabled for SI due to bug #10817
    setIfNotDefault(enable_, GPU_WAVE_LIMIT_ENABLE,
         /*owner_->workGroupInfo()->limitWave_*/ false && gpuDev->settings().ciPlus_);
}

}  // namespace pal

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <array>

class Job {
public:
    std::string jobId;
    uint64_t height;
    std::string seedHash;
    uint64_t difficulty;
    size_t nonceOffset;
    std::array<uint64_t, 4> targetHash;

    Job();
    Job(const std::string& blobHex, const std::string& id, const std::string& targetHex,
        uint64_t h, const std::string& seed);
    Job(const Job& other);
    Job& operator=(const Job& other);

    size_t findNonceOffset() const;
    std::vector<uint8_t> getBlobBytes() const;
    std::string getJobId() const;
    std::string getTarget() const;
    static std::array<uint64_t, 4> difficultyToTarget(uint64_t difficulty);
    bool isValidShare(const std::array<uint64_t, 4>& hashResult) const;
    std::string getTargetHex() const;

    // Returns a writable copy only when a caller explicitly needs one.
    // MiningHotPath uses the direct blob accessors below to avoid a copy per nonce.
    const std::vector<uint8_t>& getBlobBytesRef() const noexcept { return blob; }
    const std::array<uint64_t, 4>& getTargetHash() const noexcept { return targetHash; }

private:
    std::vector<uint8_t> blob;
};

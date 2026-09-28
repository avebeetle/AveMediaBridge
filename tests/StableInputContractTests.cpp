#include "Input/StableInputContract.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

using AveMediaBridge::Input::readSource;
using AveMediaBridge::Input::validateSource;

static_assert(sizeof(AMBI_SourceV1) == 72);
static_assert(alignof(AMBI_SourceV1) == 8);
static_assert(offsetof(AMBI_SourceV1, byteSize) == 8);
static_assert(offsetof(AMBI_SourceV1, sourceToken) == 16);
static_assert(offsetof(AMBI_SourceV1, user) == 32);
static_assert(offsetof(AMBI_SourceV1, readAt) == 40);
static_assert(offsetof(AMBI_SourceV1, checkCancel) == 48);
static_assert(offsetof(AMBI_SourceV1, reserved) == 56);

struct Fixture {
    enum class Mode { Normal, EmptyOk, ShortEof, Unknown, TooMany, Throw, Canceled, Error } mode = Mode::Normal;
    AMBI_Status cancelStatus = AMBI_OK;
    unsigned reads = 0;
    unsigned cancelChecks = 0;
    bool uninitializedCount = false;
    uint64_t lastOffset = UINT64_MAX;
    uint32_t lastRequest = 0;
};

AMBI_Status __cdecl readFixture(void* user, uint64_t offset,
    void* destination, uint32_t requested, uint32_t* bytesRead) {
    auto& fixture = *static_cast<Fixture*>(user);
    ++fixture.reads;
    fixture.lastOffset = offset;
    fixture.lastRequest = requested;
    if (bytesRead == nullptr || *bytesRead != 0) fixture.uninitializedCount = true;
    if (fixture.mode == Fixture::Mode::Throw) throw std::runtime_error("reader fault");
    if (fixture.mode == Fixture::Mode::Canceled || fixture.mode == Fixture::Mode::Error) {
        *static_cast<char*>(destination) = 'x';
        *bytesRead = 1;
        return fixture.mode == Fixture::Mode::Canceled ? AMBI_CANCELED : AMBI_IO_ERROR;
    }
    if (fixture.mode == Fixture::Mode::Unknown) return 99;
    if (fixture.mode == Fixture::Mode::TooMany) {
        *bytesRead = requested + 1;
        return AMBI_OK;
    }
    if (fixture.mode == Fixture::Mode::EmptyOk) return AMBI_OK;
    if (fixture.mode == Fixture::Mode::ShortEof) {
        *bytesRead = 1;
        return AMBI_EOF;
    }
    if (bytesRead == nullptr || (requested != 0 && destination == nullptr) ||
        offset > 5 || requested > 5 - offset) return AMBI_INVALID_ARGUMENT;
    std::memcpy(destination, "abcde" + offset, requested);
    *bytesRead = requested;
    return AMBI_OK;
}

AMBI_Status __cdecl checkFixture(void* user) {
    auto& fixture = *static_cast<Fixture*>(user);
    ++fixture.cancelChecks;
    if (fixture.cancelStatus == 99) throw std::runtime_error("cancel fault");
    return fixture.cancelStatus;
}

struct Checks {
    int executed = 0;
    int failures = 0;
    void expect(bool condition, const char* label) {
        ++executed;
        if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
    }
};

AMBI_SourceV1 sourceFor(Fixture& fixture) {
    AMBI_SourceV1 source{};
    source.structSize = sizeof(source);
    source.abiVersion = AMBI_ABI_VERSION;
    source.byteSize = 5;
    source.sourceToken[0] = 1;
    source.user = &fixture;
    source.readAt = readFixture;
    source.checkCancel = checkFixture;
    return source;
}

int main() {
    Checks check;
    Fixture fixture;
    auto source = sourceFor(fixture);
    check.expect(validateSource(&source) == AMBI_OK, "valid source");
    check.expect(validateSource(nullptr) == AMBI_INVALID_ARGUMENT, "null source");
    auto bad = source;
    bad.structSize = 0;
    check.expect(validateSource(&bad) == AMBI_INVALID_ARGUMENT, "wrong size");
    bad = source; bad.abiVersion = 2;
    check.expect(validateSource(&bad) == AMBI_INVALID_ARGUMENT, "wrong version");
    bad = source; bad.sourceToken[0] = 0;
    check.expect(validateSource(&bad) == AMBI_INVALID_ARGUMENT, "zero token");
    bad = source; bad.reserved[3] = 1;
    check.expect(validateSource(&bad) == AMBI_INVALID_ARGUMENT, "reserved nonzero");
    bad = source; bad.readAt = nullptr;
    check.expect(validateSource(&bad) == AMBI_INVALID_ARGUMENT, "missing reader");
    bad = source; bad.checkCancel = nullptr;
    check.expect(validateSource(&bad) == AMBI_INVALID_ARGUMENT, "missing cancellation callback");
    bad = source; bad.byteSize = 0;
    check.expect(validateSource(&bad) == AMBI_INVALID_ARGUMENT, "zero byte size");
    bad = source; bad.byteSize = static_cast<uint64_t>(INT64_MAX) + 1;
    check.expect(validateSource(&bad) == AMBI_INVALID_ARGUMENT, "byte size beyond signed seek");
    bad = source; bad.user = nullptr;
    check.expect(validateSource(&bad) == AMBI_OK, "stateless reader may have null user");

    auto expectRead = [&](uint64_t offset, uint32_t count, void* destination,
                          AMBI_Status expected, uint32_t expectedCount,
                          const char* label) {
        uint32_t actualCount = 77;
        const auto actual = readSource(source, offset, destination, count, actualCount);
        check.expect(actual == expected && actualCount == expectedCount, label);
    };
    char data[8]{};
    expectRead(0, 0, nullptr, AMBI_OK, 0, "empty read at start");
    expectRead(5, 0, nullptr, AMBI_OK, 0, "empty read at end");
    check.expect(fixture.reads == 0, "zero request does not dispatch");
    expectRead(1, 3, data, AMBI_OK, 3, "bounded read");
    check.expect(std::memcmp(data, "bcd", 3) == 0 && fixture.lastOffset == 1 &&
        fixture.lastRequest == 3, "bounded data and callback range");
    expectRead(3, 4, data, AMBI_EOF, 2, "crossing fixed EOF");
    check.expect(std::memcmp(data, "de", 2) == 0 && fixture.lastRequest == 2,
        "crossing read only dispatches remaining bytes");
    const auto reads = fixture.reads;
    expectRead(5, 1, data, AMBI_EOF, 0, "read at EOF");
    expectRead(6, 0, nullptr, AMBI_INVALID_ARGUMENT, 0, "empty read beyond EOF");
    expectRead(UINT64_MAX, 2, data, AMBI_INVALID_ARGUMENT, 0, "overflow offset");
    expectRead(0, AMBI_MAX_READ_BYTES + 1, data, AMBI_INVALID_ARGUMENT, 0, "oversized read");
    expectRead(0, 1, nullptr, AMBI_INVALID_ARGUMENT, 0, "null destination");
    check.expect(fixture.reads == reads, "invalid and EOF reads do not dispatch");

    fixture.mode = Fixture::Mode::EmptyOk;
    expectRead(0, 1, data, AMBI_IO_ERROR, 0, "empty OK before EOF");
    fixture.mode = Fixture::Mode::ShortEof;
    expectRead(0, 3, data, AMBI_IO_ERROR, 0, "early EOF with bytes");
    fixture.mode = Fixture::Mode::Unknown;
    expectRead(0, 1, data, AMBI_IO_ERROR, 0, "unknown reader status");
    fixture.mode = Fixture::Mode::TooMany;
    expectRead(0, 1, data, AMBI_IO_ERROR, 0, "reader count beyond request");
    fixture.mode = Fixture::Mode::Throw;
    expectRead(0, 1, data, AMBI_IO_ERROR, 0, "throwing reader");
    fixture.mode = Fixture::Mode::Canceled;
    expectRead(0, 1, data, AMBI_CANCELED, 0, "reader cancellation");
    fixture.mode = Fixture::Mode::Error;
    expectRead(0, 1, data, AMBI_IO_ERROR, 0, "reader I/O error");
    fixture.mode = Fixture::Mode::Normal;
    fixture.cancelStatus = AMBI_CANCELED;
    const auto readsBeforeCancel = fixture.reads;
    expectRead(0, 1, data, AMBI_CANCELED, 0, "cancel before dispatch");
    check.expect(fixture.reads == readsBeforeCancel, "cancel prevents read callback");
    fixture.cancelStatus = AMBI_IO_ERROR;
    expectRead(0, 1, data, AMBI_IO_ERROR, 0, "cancel query I/O error");
    fixture.cancelStatus = AMBI_EOF;
    expectRead(0, 1, data, AMBI_IO_ERROR, 0, "unexpected cancel query status");
    fixture.cancelStatus = 99;
    expectRead(0, 1, data, AMBI_IO_ERROR, 0, "throwing cancel query");
    check.expect(fixture.cancelChecks > 0, "cancellation checked");
    check.expect(!fixture.uninitializedCount, "reader receives zeroed nonnull count");
    if (check.executed == 0) {
        std::cerr << "FAIL: no contract checks executed\n";
        return 1;
    }
    if (check.failures != 0) return 1;
    std::cout << "Stable input contract: " << check.executed << " checks passed\n";
    return 0;
}

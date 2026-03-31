#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "backend/interpreter/interpreter.h"
#include "codegen/api/codegen_context.h"
#include "codegen/sql_values/sql_value.h"
#include "codegen/structures/hash_table.h"
#include "codegen/tuples/tuple_layout.h"
#include "common/types.h"
#include "ir/opcode.h"
#include "ir/program.h"

using namespace umbra::codegen;
using namespace umbra::ir;
using namespace umbra;
using umbra::interpreter::Interpreter;

// ── Helper ──────────────────────────────────────────────────────────────────

static uint64_t run(Program& program, FunctionId func, std::vector<uint64_t> args = {}) {
    Interpreter interp;
    return interp.execute(program, func, std::span<const uint64_t>(args));
}

// ── Runtime HashTable tests (no codegen) ────────────────────────────────────

TEST_CASE("HashTable: basic construction", "[hash_table]") {
    HashTable ht(16);
    REQUIRE(ht.size() == 0);
    REQUIRE(ht.capacity() == 16);
    REQUIRE(ht.entrySize() == 16);
}

TEST_CASE("HashTable: insert and lookup", "[hash_table]") {
    HashTable ht(8);

    auto* entry1 = ht.insert(100);
    REQUIRE(entry1 != nullptr);
    REQUIRE(ht.size() == 1);

    // Write some data to the entry.
    int64_t val = 42;
    std::memcpy(entry1, &val, sizeof(val));

    // Lookup should find it.
    auto* found = ht.lookup(100);
    REQUIRE(found != nullptr);
    REQUIRE(found == entry1);

    int64_t readVal = 0;
    std::memcpy(&readVal, found, sizeof(readVal));
    REQUIRE(readVal == 42);
}

TEST_CASE("HashTable: lookup miss returns nullptr", "[hash_table]") {
    HashTable ht(8);
    ht.insert(100);
    auto* found = ht.lookup(200);
    REQUIRE(found == nullptr);
}

TEST_CASE("HashTable: multiple inserts", "[hash_table]") {
    HashTable ht(8);

    auto* e1 = ht.insert(10);
    auto* e2 = ht.insert(20);
    auto* e3 = ht.insert(30);

    REQUIRE(ht.size() == 3);
    REQUIRE(e1 != e2);
    REQUIRE(e2 != e3);

    // All should be findable.
    REQUIRE(ht.lookup(10) == e1);
    REQUIRE(ht.lookup(20) == e2);
    REQUIRE(ht.lookup(30) == e3);
}

TEST_CASE("HashTable: hash collision handling", "[hash_table]") {
    HashTable ht(8);

    // Insert entries that will collide (same low bits, different hash).
    // Capacity is 16, so slot = hash & 15.
    // hash=16 and hash=0 both map to slot 0.
    auto* e1 = ht.insert(0);
    auto* e2 = ht.insert(16);

    REQUIRE(e1 != e2);
    REQUIRE(ht.size() == 2);

    // Both should be findable via lookup + probeNext.
    auto* found1 = ht.lookup(0);
    REQUIRE(found1 == e1);

    auto* found2 = ht.lookup(16);
    REQUIRE(found2 == e2);
}

TEST_CASE("HashTable: probeNext for hash collisions", "[hash_table]") {
    HashTable ht(8);

    // Insert entries with same hash.
    auto* e1 = ht.insert(42);
    auto* e2 = ht.insert(42);

    REQUIRE(e1 != e2);

    // Lookup returns first match.
    auto* found = ht.lookup(42);
    REQUIRE(found == e1);

    // ProbeNext from first entry finds second.
    auto* next = ht.probeNext(found, 42);
    REQUIRE(next == e2);

    // ProbeNext from second entry returns nullptr.
    auto* end = ht.probeNext(next, 42);
    REQUIRE(end == nullptr);
}

TEST_CASE("HashTable: resize on high load", "[hash_table]") {
    HashTable ht(8);
    auto initialCap = ht.capacity();

    // Insert enough entries to trigger resize (> 70% load).
    for (uint64_t i = 0; i < 12; ++i) {
        ht.insert(i * 1000 + 1);
    }

    REQUIRE(ht.size() == 12);
    REQUIRE(ht.capacity() > initialCap);

    // All entries should still be findable after resize.
    for (uint64_t i = 0; i < 12; ++i) {
        REQUIRE(ht.lookup(i * 1000 + 1) != nullptr);
    }
}

TEST_CASE("HashTable: iterate visits all entries", "[hash_table]") {
    HashTable ht(sizeof(int64_t));

    // Insert 5 entries with values 10, 20, 30, 40, 50.
    for (int64_t i = 1; i <= 5; ++i) {
        auto* entry = ht.insert(static_cast<uint64_t>(i * 100));
        int64_t val = i * 10;
        std::memcpy(entry, &val, sizeof(val));
    }

    // Iterate and sum all values.
    int64_t sum = 0;
    ht.iterate(
        [](uint8_t* entry, void* userData) {
            int64_t val = 0;
            std::memcpy(&val, entry, sizeof(val));
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            *reinterpret_cast<int64_t*>(userData) += val;
        },
        &sum);

    REQUIRE(sum == 10 + 20 + 30 + 40 + 50);
}

TEST_CASE("HashTable: host function lookup", "[hash_table]") {
    HashTable ht(8);
    auto* entry = ht.insert(42);
    int64_t val = 999;
    std::memcpy(entry, &val, sizeof(val));

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    uint64_t args[] = {reinterpret_cast<uint64_t>(&ht), 42};
    auto result = HashTable::hostLookup(args, 2);
    REQUIRE(result != 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* resultPtr = reinterpret_cast<uint8_t*>(result);
    int64_t readVal = 0;
    std::memcpy(&readVal, resultPtr, sizeof(readVal));
    REQUIRE(readVal == 999);
}

TEST_CASE("HashTable: host function insert", "[hash_table]") {
    HashTable ht(8);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    uint64_t args[] = {reinterpret_cast<uint64_t>(&ht), 77};
    auto result = HashTable::hostInsert(args, 2);
    REQUIRE(result != 0);
    REQUIRE(ht.size() == 1);
}

TEST_CASE("HashTable: host function probeNext", "[hash_table]") {
    HashTable ht(8);
    auto* e1 = ht.insert(42);
    ht.insert(42);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    uint64_t args[] = {reinterpret_cast<uint64_t>(&ht), reinterpret_cast<uint64_t>(e1), 42};
    auto result = HashTable::hostProbeNext(args, 3);
    REQUIRE(result != 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    REQUIRE(reinterpret_cast<uint8_t*>(result) != e1);
}

TEST_CASE("HashTable: host function getCapacity", "[hash_table]") {
    HashTable ht(8);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    uint64_t args[] = {reinterpret_cast<uint64_t>(&ht)};
    REQUIRE(HashTable::hostGetCapacity(args, 1) == 16);
}

TEST_CASE("HashTable: host function getSlotBuffer", "[hash_table]") {
    HashTable ht(8);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    uint64_t args[] = {reinterpret_cast<uint64_t>(&ht)};
    auto result = HashTable::hostGetSlotBuffer(args, 1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    REQUIRE(reinterpret_cast<uint8_t*>(result) == ht.slotBuffer());
}

TEST_CASE("HashTable: host function getSlotStride", "[hash_table]") {
    HashTable ht(8);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    uint64_t args[] = {reinterpret_cast<uint64_t>(&ht)};
    REQUIRE(HashTable::hostGetSlotStride(args, 1) == 9 + 8);  // PAYLOAD_OFFSET + entrySize
}

TEST_CASE("HashTable: move constructor", "[hash_table]") {
    HashTable ht1(8);
    ht1.insert(42);
    REQUIRE(ht1.size() == 1);

    HashTable ht2(std::move(ht1));
    REQUIRE(ht2.size() == 1);
    REQUIRE(ht2.lookup(42) != nullptr);
}

TEST_CASE("HashTable: move assignment", "[hash_table]") {
    HashTable ht1(8);
    ht1.insert(42);

    HashTable ht2(16);
    ht2 = std::move(ht1);
    REQUIRE(ht2.size() == 1);
    REQUIRE(ht2.lookup(42) != nullptr);
}

// ── Codegen-based HashTable tests ───────────────────────────────────────────

TEST_CASE("HashTableCodegen: findOrInsert new entry", "[hash_table]") {
    // Create a hash table with key=Int8 (one byte), agg=Int64 (8 bytes).
    std::vector<SQLType> keyTypes = {SQLType::boolean()};
    std::vector<SQLType> aggTypes = {SQLType::bigint()};

    HashTableCodegen codegen(keyTypes, aggTypes);
    HashTable ht(codegen.entrySize());

    CodegenContext ctx;
    setActiveContext(&ctx);
    codegen.registerHostFunctions(ctx);

    // fn(tablePtr, keyVal) -> ptr: findOrInsert with the given key.
    auto fn = ctx.createFunction("find_or_insert", TypeId::PTR, {TypeId::PTR, TypeId::INT8});
    auto tablePtr = ctx.getArg<Ptr<Int8>>(0);
    auto keyOp = ctx.builder().emitFunArg(TypeId::INT8, 1);
    SQLValue keyVal(ctx, keyOp, SQLType::boolean());

    auto hashVal = TupleLayout::hash(ctx, {keyVal});
    auto entry = codegen.findOrInsert(ctx, tablePtr, hashVal, {keyVal});
    ctx.emitRet(entry);

    auto program = ctx.finalize();

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto tableAddr = reinterpret_cast<uint64_t>(&ht);

    // First call with key='A' should insert.
    auto result1 = run(program, fn, {tableAddr, 'A'});
    REQUIRE(result1 != 0);
    REQUIRE(ht.size() == 1);

    // Second call with same key should find existing.
    auto result2 = run(program, fn, {tableAddr, 'A'});
    REQUIRE(result2 == result1);
    REQUIRE(ht.size() == 1);

    // Third call with different key should insert new.
    auto result3 = run(program, fn, {tableAddr, 'B'});
    REQUIRE(result3 != 0);
    REQUIRE(result3 != result1);
    REQUIRE(ht.size() == 2);
}

TEST_CASE("HashTableCodegen: findOrInsert with aggregate accumulation", "[hash_table]") {
    // Simulate Q1-like pattern: group by Int8 key, accumulate Int64 sum.
    std::vector<SQLType> keyTypes = {SQLType::boolean()};
    std::vector<SQLType> aggTypes = {SQLType::bigint()};

    HashTableCodegen codegen(keyTypes, aggTypes);
    HashTable ht(codegen.entrySize());

    CodegenContext ctx;
    setActiveContext(&ctx);
    codegen.registerHostFunctions(ctx);

    // fn(tablePtr, keyVal, addVal): findOrInsert, then add addVal to the agg state.
    auto fn = ctx.createFunction("agg_accumulate", TypeId::VOID,
                                 {TypeId::PTR, TypeId::INT8, TypeId::INT64});
    auto tablePtr = ctx.getArg<Ptr<Int8>>(0);
    auto keyOp = ctx.builder().emitFunArg(TypeId::INT8, 1);
    auto addOp = ctx.builder().emitFunArg(TypeId::INT64, 2);
    SQLValue keyVal(ctx, keyOp, SQLType::boolean());

    auto hashVal = TupleLayout::hash(ctx, {keyVal});
    auto entry = codegen.findOrInsert(ctx, tablePtr, hashVal, {keyVal});

    // Compute pointer to agg state (after key layout).
    auto aggOff = ctx.constInt64(static_cast<int64_t>(codegen.aggOffset()));
    auto aggPtr = Ptr<Int8>(ctx.builder().emitGEP(entry.op(), TypeId::INT8, aggOff.op()));
    auto typedAggPtr = aggPtr.bitcastTo<Int64>(ctx);

    // Load current sum, add, store back.
    auto currentSum = typedAggPtr.load(ctx);
    auto addVal = Int64(addOp);
    auto newSum = currentSum + addVal;
    typedAggPtr.store(ctx, newSum);

    ctx.emitRetVoid();

    auto program = ctx.finalize();

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto tableAddr = reinterpret_cast<uint64_t>(&ht);

    // Accumulate: key='A' with values 10, 20, 30.
    run(program, fn, {tableAddr, 'A', 10});
    run(program, fn, {tableAddr, 'A', 20});
    run(program, fn, {tableAddr, 'A', 30});

    // Accumulate: key='B' with values 100, 200.
    run(program, fn, {tableAddr, 'B', 100});
    run(program, fn, {tableAddr, 'B', 200});

    REQUIRE(ht.size() == 2);

    // Verify sums by iterating.
    struct IterData {
        const HashTableCodegen* codegen;
        int64_t sumA = 0;
        int64_t sumB = 0;
    } iterData{&codegen};

    ht.iterate(
        [](uint8_t* entry, void* userData) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            auto* data = reinterpret_cast<IterData*>(userData);
            auto keyOffset = data->codegen->keyLayout().field(0).offset;
            auto key = entry[keyOffset];
            auto aggOff = data->codegen->aggOffset();
            int64_t sum = 0;
            std::memcpy(&sum, entry + aggOff, sizeof(sum));
            if (key == 'A') {
                data->sumA = sum;
            } else if (key == 'B') {
                data->sumB = sum;
            }
        },
        &iterData);

    REQUIRE(iterData.sumA == 60);
    REQUIRE(iterData.sumB == 300);
}

TEST_CASE("HashTableCodegen: scan iterates all entries", "[hash_table]") {
    // Insert entries via runtime API, then use codegen scan to sum them.
    std::vector<SQLType> keyTypes = {SQLType::boolean()};
    std::vector<SQLType> aggTypes = {SQLType::bigint()};

    HashTableCodegen codegen(keyTypes, aggTypes);
    HashTable ht(codegen.entrySize());

    // Manually insert entries using the runtime API.
    auto* e1 = ht.insert(100);
    auto* e2 = ht.insert(200);

    // Write key + agg state for each.
    // Entry layout: [key(1B)] [null_bitmap(1B)] [sum(8B)] [null_bitmap(1B)]
    e1[0] = 'A';  // key
    e1[1] = 0;    // null bitmap for key
    int64_t sum1 = 1000;
    auto aggOff = codegen.aggOffset();
    std::memcpy(e1 + aggOff, &sum1, sizeof(sum1));

    e2[0] = 'B';  // key
    e2[1] = 0;    // null bitmap for key
    int64_t sum2 = 2000;
    std::memcpy(e2 + aggOff, &sum2, sizeof(sum2));

    CodegenContext ctx;
    setActiveContext(&ctx);
    codegen.registerHostFunctions(ctx);

    // fn(tablePtr, outPtr, startSlot, endSlot): scan the table, sum all agg values, write to outPtr.
    auto fn = ctx.createFunction("scan_sum", TypeId::VOID,
                                 {TypeId::PTR, TypeId::PTR, TypeId::INT64, TypeId::INT64});
    auto tablePtr = ctx.getArg<Ptr<Int8>>(0);
    auto outPtr = ctx.getArg<Ptr<Int64>>(1);
    auto startSlot = ctx.getArg<Int64>(2);
    auto endSlot = ctx.getArg<Int64>(3);

    // Initialize sum to 0.
    auto zero = ctx.constInt64(0);
    outPtr.store(ctx, zero);

    codegen.scan(ctx, tablePtr, startSlot, endSlot, [&](Ptr<Int8> entry) {
        // Load the aggregate state from entry.
        auto aggOffset = ctx.constInt64(static_cast<int64_t>(codegen.aggOffset()));
        auto statePtr = Ptr<Int8>(ctx.builder().emitGEP(entry.op(), TypeId::INT8, aggOffset.op()));
        auto typedPtr = statePtr.bitcastTo<Int64>(ctx);
        auto val = typedPtr.load(ctx);

        // Add to running sum.
        auto currentSum = outPtr.load(ctx);
        auto newSum = currentSum + val;
        outPtr.store(ctx, newSum);
    });

    ctx.emitRetVoid();

    auto program = ctx.finalize();

    int64_t output = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto tableAddr = reinterpret_cast<uint64_t>(&ht);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto outAddr = reinterpret_cast<uint64_t>(&output);
    run(program, fn, {tableAddr, outAddr, 0, ht.capacity()});

    REQUIRE(output == 3000);
}

TEST_CASE("HashTableCodegen: entrySize and aggOffset", "[hash_table]") {
    std::vector<SQLType> keyTypes = {SQLType::boolean(), SQLType::boolean()};
    std::vector<SQLType> aggTypes = {SQLType::bigint(), SQLType::bigint()};

    HashTableCodegen codegen(keyTypes, aggTypes);

    // Key layout: 2 x Int8 + 1 byte null bitmap = 3 bytes
    REQUIRE(codegen.keyLayout().totalSize() == 3);
    // Agg layout: 2 x Int64 + 1 byte null bitmap = 17 bytes
    REQUIRE(codegen.aggLayout().totalSize() == 17);
    // Entry = key + agg = 3 + 17 = 20
    REQUIRE(codegen.entrySize() == 20);
    REQUIRE(codegen.aggOffset() == 3);
}
